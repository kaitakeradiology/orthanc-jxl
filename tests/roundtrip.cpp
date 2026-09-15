/*
 * End-to-end roundtrip test for the JXL transcoder.
 *
 * For each uncompressed DICOM file given on the command line, this runs the
 * exact production transcode path (native -> JXL -> native) and verifies the
 * recovered pixel data is byte-identical to the input (after the planar ->
 * interleaved normalisation the encoder performs). It also checks the frame
 * count survives the roundtrip.
 *
 * Usage: roundtrip <dicom_file> [<dicom_file> ...]
 */

#include "../src/transcode.h"
#include "../src/dicom_handler.h"
#include "../src/transfer_syntax.h"
#include "../src/pixel_layout.h"
#include "../src/config.h"
#include "../src/thread_pool.h"

#include <dcmtk/dcmjpeg/djdecode.h>
#include <dcmtk/dcmjpls/djdecode.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace orthanc_jxl;

static std::vector<uint8_t> ReadFile(const char* path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        throw std::runtime_error(std::string("Failed to open: ") + path);
    }
    size_t size = file.tellg();
    file.seekg(0);
    std::vector<uint8_t> data(size);
    file.read(reinterpret_cast<char*>(data.data()), size);
    return data;
}

// Reproduce the encoder's planar -> interleaved normalisation so the expected
// recovered buffer matches what the transcoder will have stored.
static std::vector<uint8_t> ExpectedRecovered(const DicomImageInfo& info,
                                              const std::vector<uint8_t>& orig) {
    const bool planar = (info.planarConfiguration == 1 && info.samplesPerPixel > 1);
    if (!planar) {
        return orig;
    }
    const size_t frameSize = info.FrameSizeBytes();
    const int channels = info.samplesPerPixel;
    const int bytesPerSample = (info.bitsAllocated + 7) / 8;
    const size_t pixelsPerFrame = static_cast<size_t>(info.width) * info.height;
    std::vector<uint8_t> out;
    out.reserve(orig.size());
    for (uint32_t f = 0; f < info.numberOfFrames; ++f) {
        auto frame = PlanarToInterleaved(orig.data() + f * frameSize,
                                         pixelsPerFrame, channels, bytesPerSample);
        out.insert(out.end(), frame.begin(), frame.end());
    }
    return out;
}

// Lossless roundtrip check (native -> JXL -> native), shared by the
// file-based test and the lossless sanity check of the synthetic signed
// fixture below. `label` is only used for the printed report line.
static bool RunOneBytes(const char* label, const std::vector<uint8_t>& dicom,
                        ThreadPool& pool) {
    DicomImageInfo info;
    std::vector<uint8_t> origPixels;
    {
        DicomHandler handler(dicom.data(), dicom.size());
        info = handler.GetImageInfo();
        handler.EnsureUncompressed();   // decode compressed sources so the reference is native
        origPixels = handler.GetPixelData();
    }

    PluginConfig config = PluginConfig::Default();  // ProgressiveLossless

    TranscodeResult toJxl = TranscodeToJxl(dicom.data(), dicom.size(), config, pool);

    // The encoded instance must advertise a JXL transfer syntax and the same
    // number of frames we started with.
    std::string jxlTs;
    uint32_t encFrames = 0;
    {
        DicomHandler jxlHandler(toJxl.dicom.data(), toJxl.dicom.size());
        jxlTs = jxlHandler.GetTransferSyntax();
        encFrames = jxlHandler.GetEncapsulatedFrameCount();
    }

    TranscodeResult fromJxl = TranscodeFromJxl(
        toJxl.dicom.data(), toJxl.dicom.size(), TS_LITTLE_ENDIAN_EXPLICIT, pool);

    std::vector<uint8_t> rtPixels;
    {
        DicomHandler rtHandler(fromJxl.dicom.data(), fromJxl.dicom.size());
        rtPixels = rtHandler.GetPixelData();
    }

    std::vector<uint8_t> expected = ExpectedRecovered(info, origPixels);

    // A lossless config must produce .110, never the lossy .112 - this exact
    // combination once mislabeled lossy bits, see VerifyLossyRewrite below.
    bool tsOk = (jxlTs == TS_JPEG_XL_LOSSLESS);
    bool framesOk = (encFrames == info.numberOfFrames) &&
                    (fromJxl.frameCount == info.numberOfFrames);
    bool sizeOk = (rtPixels.size() == expected.size());
    bool bytesOk = sizeOk && (rtPixels == expected);
    bool pass = tsOk && framesOk && bytesOk;

    double ratio = toJxl.encodedBytes
        ? static_cast<double>(toJxl.nativeBytes) / toJxl.encodedBytes : 0.0;

    printf("%-40s %3ux%-3u f=%-3u spp=%u ba=%-2u %-14s planar=%u  %5.2fx  %s\n",
           label, info.width, info.height, info.numberOfFrames,
           info.samplesPerPixel, info.bitsAllocated,
           info.photometricInterpretation.c_str(), info.planarConfiguration,
           ratio, pass ? "PASS" : "FAIL");

    if (!pass) {
        if (!tsOk)     printf("    -> bad transfer syntax: %s\n", jxlTs.c_str());
        if (!framesOk) printf("    -> frame count mismatch: enc=%u from=%u expected=%u\n",
                              encFrames, fromJxl.frameCount, info.numberOfFrames);
        if (!sizeOk)   printf("    -> size mismatch: got %zu expected %zu\n",
                              rtPixels.size(), expected.size());
        else if (!bytesOk) printf("    -> NOT lossless: pixel bytes differ\n");
    }
    return pass;
}

static bool RunOne(const char* path, ThreadPool& pool) {
    return RunOneBytes(path, ReadFile(path), pool);
}

// Build a signed 16-bit variant of an uncompressed source: shift every
// sample down by kShift (so the encoded range spans both signs, exercising
// the sign-offset rewrite meaningfully) and widen BitsStored/HighBit to the
// full 16-bit word - real signed CT almost always does this too, precisely
// to avoid the sub-word two's-complement packing DicomImageInfo::isSigned
// callers otherwise have to reason about. RescaleIntercept is adjusted so
// the recovered HU values are identical to the source's, and a synthetic
// PixelPaddingValue is added (at the shifted range's low end) purely to
// exercise that tag's rewrite path too.
static std::vector<uint8_t> MakeSyntheticSigned(const std::vector<uint8_t>& srcDicom,
                                                double existingIntercept) {
    constexpr int32_t kShift = 2048;

    DicomHandler handler(srcDicom.data(), srcDicom.size());
    handler.EnsureUncompressed();
    std::vector<uint8_t> pixels = handler.GetPixelData();

    uint16_t* samples = reinterpret_cast<uint16_t*>(pixels.data());
    const size_t count = pixels.size() / 2;
    for (size_t i = 0; i < count; ++i) {
        const int32_t v = static_cast<int32_t>(samples[i]) - kShift;
        samples[i] = static_cast<uint16_t>(static_cast<int16_t>(v));
    }
    handler.SetNativePixelData(pixels);

    handler.SetUint16(0x0028, 0x0103, 1);   // PixelRepresentation = signed
    handler.SetUint16(0x0028, 0x0101, 16);  // BitsStored
    handler.SetUint16(0x0028, 0x0102, 15);  // HighBit

    char interceptBuf[32];
    std::snprintf(interceptBuf, sizeof(interceptBuf), "%.0f", existingIntercept + kShift);
    handler.SetString(0x0028, 0x1052, interceptBuf);  // RescaleIntercept

    handler.SetSint16(0x0028, 0x0120, static_cast<int16_t>(-kShift));  // PixelPaddingValue

    return handler.WriteToBuffer(TS_LITTLE_ENDIAN_EXPLICIT);
}

// Comprehensive check of the lossy TO-JXL rewrite (dicom_handler.cpp /
// transcode.cpp ApplyLossyTags): transfer syntax, every PS3.3 C.7.6.1.1.5
// lossy tag, the signed->unsigned pixel/tag rewrite, new SOPInstanceUID
// bookkeeping, and that the HU values recovered from the lossy bits are
// still close to the source's. Also re-checks the synthetic signed fixture
// through the LOSSLESS path (RunOneBytes) to confirm the sign-offset rewrite
// never engages there.
static bool VerifyLossyRewrite(const char* path, ThreadPool& pool) {
    bool allOk = true;
    auto dicom = ReadFile(path);

    double origIntercept = 0.0, origSlope = 1.0;
    std::vector<uint8_t> origPixels;
    {
        DicomHandler h(dicom.data(), dicom.size());
        h.EnsureUncompressed();
        origPixels = h.GetPixelData();
        std::string s;
        if (h.GetString(0x0028, 0x1052, s)) origIntercept = std::atof(s.c_str());
        if (h.GetString(0x0028, 0x1053, s)) origSlope = std::atof(s.c_str());
    }

    auto signedDicom = MakeSyntheticSigned(dicom, origIntercept);

    std::string origSopUid;
    {
        DicomHandler h(signedDicom.data(), signedDicom.size());
        h.GetString(0x0008, 0x0018, origSopUid);
    }

    // The lossless path must stay bit-exact for signed data too - the
    // sign-offset rewrite is gated on `lossy` in transcode.cpp and must never
    // engage here.
    if (!RunOneBytes("synthetic-signed (lossless)", signedDicom, pool)) {
        allOk = false;
    }

    const float kDistance = 0.5f;
    PluginConfig config = PluginConfig::Default();
    config.encodeOptions.mode = EncodeMode::ProgressiveVarDCT;
    config.encodeOptions.distance = kDistance;

    TranscodeResult lossy = TranscodeToJxl(signedDicom.data(), signedDicom.size(), config, pool);

    DicomHandler out(lossy.dicom.data(), lossy.dicom.size());
    const std::string ts = out.GetTransferSyntax();
    const DicomImageInfo outInfo = out.GetImageInfo();

    std::string lossyFlag, method, ratioStr, imageType, sopUid, mediaSopUid, interceptStr;
    out.GetString(0x0028, 0x2110, lossyFlag);
    out.GetString(0x0028, 0x2114, method);
    out.GetString(0x0028, 0x2112, ratioStr);
    out.GetString(0x0008, 0x0008, imageType);
    out.GetString(0x0008, 0x0018, sopUid);
    out.GetMetaString(0x0002, 0x0003, mediaSopUid);
    out.GetString(0x0028, 0x1052, interceptStr);

    uint16_t paddingOut = 0;
    const bool paddingReadable = out.GetUint16(0x0028, 0x0120, paddingOut);

    const uint32_t offset = 1u << 15;  // BitsStored=16 in the synthetic fixture
    const double expectedIntercept = (origIntercept + 2048.0) - static_cast<double>(offset);
    const double gotIntercept = std::atof(interceptStr.c_str());
    const uint16_t expectedPadding = static_cast<uint16_t>(-2048 + static_cast<int32_t>(offset));

    struct Check { const char* name; bool ok; std::string detail; };
    std::vector<Check> checks;
    checks.push_back({"transfer syntax .112", ts == TS_JPEG_XL, ts});
    checks.push_back({"PixelRepresentation unsigned", !outInfo.isSigned, ""});
    checks.push_back({"LossyImageCompression=01", lossyFlag == "01", lossyFlag});
    checks.push_back({"LossyImageCompressionMethod", method == "ISO_18181_1", method});
    checks.push_back({"LossyImageCompressionRatio present", !ratioStr.empty() && std::atof(ratioStr.c_str()) > 1.0, ratioStr});
    checks.push_back({"ImageType[0]=DERIVED",
        imageType.rfind("DERIVED", 0) == 0 &&
        (imageType.size() == 7 || imageType[7] == '\\'), imageType});
    checks.push_back({"SOPInstanceUID changed", !sopUid.empty() && sopUid != origSopUid, sopUid});
    checks.push_back({"MediaStorageSOPInstanceUID in sync", sopUid == mediaSopUid, mediaSopUid});
    checks.push_back({"RescaleIntercept shifted by -offset",
        std::fabs(gotIntercept - expectedIntercept) < 0.5, interceptStr});
    checks.push_back({"PixelPaddingValue re-typed US + offset",
        paddingReadable && paddingOut == expectedPadding, std::to_string(paddingOut)});

    for (const auto& c : checks) {
        if (!c.ok) allOk = false;
        printf("    %-38s %-20s %s\n", c.name, c.detail.c_str(), c.ok ? "PASS" : "FAIL");
    }

    // Smallest/LargestImagePixelValue must be gone (they described the old
    // signed range and were never recomputed).
    uint16_t discard = 0;
    bool smallestGone = !out.GetUint16(0x0028, 0x0106, discard);
    bool largestGone = !out.GetUint16(0x0028, 0x0107, discard);
    printf("    %-38s %-20s %s\n", "Smallest/LargestImagePixelValue removed", "",
           (smallestGone && largestGone) ? "PASS" : "FAIL");
    if (!smallestGone || !largestGone) allOk = false;

    // Decode the lossy bits back through the plugin's own FROM-JXL path and
    // compare recovered HU values against the ORIGINAL (pre-shift) source.
    TranscodeResult fromLossy = TranscodeFromJxl(
        lossy.dicom.data(), lossy.dicom.size(), TS_LITTLE_ENDIAN_EXPLICIT, pool);
    DicomHandler decodedHandler(fromLossy.dicom.data(), fromLossy.dicom.size());
    std::vector<uint8_t> decodedPixels = decodedHandler.GetPixelData();

    std::string decSlopeStr = "1", decInterceptStr = "0";
    decodedHandler.GetString(0x0028, 0x1053, decSlopeStr);
    decodedHandler.GetString(0x0028, 0x1052, decInterceptStr);
    const double decSlope = std::atof(decSlopeStr.c_str());
    const double decIntercept = std::atof(decInterceptStr.c_str());

    const uint16_t* decSamples = reinterpret_cast<const uint16_t*>(decodedPixels.data());
    const uint16_t* origSamples = reinterpret_cast<const uint16_t*>(origPixels.data());
    const size_t n = std::min(decodedPixels.size(), origPixels.size()) / 2;

    double sumSq = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double huDecoded = static_cast<double>(decSamples[i]) * decSlope + decIntercept;
        const double huOrig = static_cast<double>(origSamples[i]) * origSlope + origIntercept;
        const double diff = huDecoded - huOrig;
        sumSq += diff * diff;
    }
    const double rmse = (n > 0) ? std::sqrt(sumSq / static_cast<double>(n)) : 0.0;

    // Threshold derived from the gateway's own measurement at the same
    // distance (d=0.5 -> RMSE~41 for the offset/sRGB-TF encoding on the
    // reference 512x512 CT); 60 leaves headroom for a different image.
    const double kRmseThreshold = 60.0;
    const bool rmseOk = rmse < kRmseThreshold;
    printf("    %-38s RMSE=%-14.2f %s\n", "recovered HU vs. original", rmse,
           rmseOk ? "PASS" : "FAIL");
    if (!rmseOk) allOk = false;

    printf("%-40s d=%.1f -> %s  %s\n\n", path, kDistance, ts.c_str(),
           allOk ? "PASS" : "FAIL");
    return allOk;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <dicom_file> [<dicom_file> ...]\n", argv[0]);
        return 2;
    }

    unsigned hw = std::thread::hardware_concurrency();
    ThreadPool pool(hw == 0 ? 1u : hw);

    // Register DCMTK decoders so compressed fixtures (JPEG / JPEG-LS) decode to
    // native before JXL-encoding (mirrors OrthancPluginInitialize).
    DJDecoderRegistration::registerCodecs();
    DJLSDecoderRegistration::registerCodecs();

    int failures = 0;
    for (int i = 1; i < argc; ++i) {
        try {
            if (!RunOne(argv[i], pool)) {
                ++failures;
            }
        } catch (const std::exception& e) {
            printf("%-40s  ERROR: %s\n", argv[i], e.what());
            ++failures;
        }
    }

    // Lossy TO-JXL rewrite check (uses the first input file).
    printf("\n");
    try {
        if (!VerifyLossyRewrite(argv[1], pool)) {
            ++failures;
        }
    } catch (const std::exception& e) {
        printf("lossy-rewrite  ERROR: %s\n", e.what());
        ++failures;
    }

    printf("\n%s\n", failures == 0 ? "ALL PASSED" : "FAILURES PRESENT");
    return failures == 0 ? 0 : 1;
}
