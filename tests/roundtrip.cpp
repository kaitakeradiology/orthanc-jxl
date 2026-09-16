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

// Build a signed 16-bit variant of an uncompressed source WITHOUT
// RescaleIntercept - transcode.cpp only selects the legacy offset scheme
// (vs. HU13) when RescaleIntercept is absent, so this fixture must strip it
// (test_ct1.dcm carries one) to actually exercise that path. Shifts every
// sample down by kShift (so the encoded range spans both signs, exercising
// the sign-offset rewrite meaningfully) and widens BitsStored/HighBit to the
// full 16-bit word - real signed CT almost always does this too, precisely
// to avoid the sub-word two's-complement packing DicomImageInfo::isSigned
// callers otherwise have to reason about. A synthetic PixelPaddingValue is
// added (at the shifted range's low end) purely to exercise that tag's
// rewrite path too.
static std::vector<uint8_t> MakeSyntheticSigned(const std::vector<uint8_t>& srcDicom) {
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

    // No RescaleIntercept/Slope - the presence of RescaleIntercept is what
    // selects HU13 over this legacy scheme in transcode.cpp.
    handler.RemoveTag(0x0028, 0x1052);
    handler.RemoveTag(0x0028, 0x1053);

    handler.SetSint16(0x0028, 0x0120, static_cast<int16_t>(-kShift));  // PixelPaddingValue

    return handler.WriteToBuffer(TS_LITTLE_ENDIAN_EXPLICIT);
}

// Comprehensive check of the LEGACY (no-RescaleIntercept) lossy TO-JXL
// rewrite (dicom_handler.cpp / transcode.cpp ApplyOffsetSchemeTags):
// transfer syntax, every PS3.3 C.7.6.1.1.5 lossy tag, the signed->unsigned
// pixel/tag rewrite, new SOPInstanceUID bookkeeping, and that the values
// recovered from the lossy bits are still close to the source's. Also
// re-checks the synthetic signed fixture through the LOSSLESS path
// (RunOneBytes) to confirm the sign-offset rewrite never engages there.
// See VerifyHu13Rewrite below for the RescaleIntercept-present case, which
// this fixture deliberately does NOT exercise (that's what selects HU13
// instead of this scheme in transcode.cpp).
static bool VerifyLossyRewrite(const char* path, ThreadPool& pool) {
    bool allOk = true;
    auto dicom = ReadFile(path);

    auto signedDicom = MakeSyntheticSigned(dicom);

    // Ground truth for the RMSE check below is the signed FIXTURE's own
    // pre-encode pixel data (not the original file's - there is no rescale
    // relating the two here), read as genuine signed samples.
    std::vector<uint8_t> origPixels;
    {
        DicomHandler h(signedDicom.data(), signedDicom.size());
        origPixels = h.GetPixelData();
    }

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
    // No source RescaleIntercept on this fixture (see MakeSyntheticSigned),
    // so ApplyOffsetSchemeTags' "existingIntercept" defaults to 0.
    const double expectedIntercept = -static_cast<double>(offset);
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
    checks.push_back({"RescaleIntercept = -offset (no source intercept)",
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
    // compare the recovered value (code * RescaleSlope + RescaleIntercept)
    // against the ORIGINAL SIGNED sample the fixture encoded (read as
    // genuine two's complement - BitsStored=16 here so ExtractStoredValue's
    // masking is a no-op, but the sign interpretation is not).
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
        const double recovered = static_cast<double>(decSamples[i]) * decSlope + decIntercept;
        const double original =
            static_cast<double>(ExtractStoredValue<uint16_t>(origSamples[i], 16, /*isSigned=*/true));
        const double diff = recovered - original;
        sumSq += diff * diff;
    }
    const double rmse = (n > 0) ? std::sqrt(sumSq / static_cast<double>(n)) : 0.0;

    // Threshold derived from the gateway's own measurement at the same
    // distance (d=0.5 -> RMSE~41 for the offset/sRGB-TF encoding on the
    // reference 512x512 CT); 60 leaves headroom for a different image.
    const double kRmseThreshold = 60.0;
    const bool rmseOk = rmse < kRmseThreshold;
    printf("    %-38s RMSE=%-14.2f %s\n", "recovered value vs. original", rmse,
           rmseOk ? "PASS" : "FAIL");
    if (!rmseOk) allOk = false;

    printf("%-40s d=%.1f -> %s  %s\n\n", path, kDistance, ts.c_str(),
           allOk ? "PASS" : "FAIL");
    return allOk;
}

// --- HU13 (rescaled) lossy rewrite ------------------------------------------

// Layout of the three deterministic blocks MakeSyntheticHu13Fixture pokes
// into the top-left of the image (rows 0..kHu13BlockSize), each separated by
// a gap of untouched original CT texture so a block's own hard edge doesn't
// sit directly against another block's - keeps VarDCT ringing at one block's
// boundary from contaminating another block's assertions.
constexpr uint32_t kHu13BlockSize = 48;
constexpr uint32_t kHu13BlockGap = 32;
constexpr uint32_t kHu13AirX = 0;
constexpr uint32_t kHu13BoneX = kHu13AirX + kHu13BlockSize + kHu13BlockGap;   // 80
constexpr uint32_t kHu13PadX = kHu13BoneX + kHu13BlockSize + kHu13BlockGap;   // 160
constexpr uint32_t kHu13BlockMargin = 8;  // inset for the strict per-pixel checks, to dodge a block's own edge ringing

constexpr int32_t kHu13AirStored = 0;      // HU -1024 given test_ct1.dcm's RescaleIntercept -1024
constexpr int32_t kHu13BoneStored = 3024;  // HU 2000
constexpr int32_t kHu13PadStored = -2000;  // synthetic PixelPaddingValue

// Build the HU13 test fixture from an unsigned, non-padding-tagged CT source
// (test_ct1.dcm ships PixelRepresentation=0 with no PixelPaddingValue, but
// DOES carry RescaleIntercept -1024 - exactly the condition that selects
// HU13 in transcode.cpp). Widens BitsStored/HighBit to a full 16-bit signed
// word (same reasoning as MakeSyntheticSigned above - avoids reasoning about
// sub-word two's-complement packing) and carves out the three blocks above
// so the assertions in VerifyHu13Rewrite have known ground truth: air at HU
// -1024 (stored 0), bone at HU 2000 (stored 3024), and a padding block at
// the synthetic PixelPaddingValue -2000. RescaleIntercept/Slope (-1024 / 1)
// come from the fixture itself and are left untouched.
static std::vector<uint8_t> MakeSyntheticHu13Fixture(const std::vector<uint8_t>& srcDicom) {
    DicomHandler handler(srcDicom.data(), srcDicom.size());
    handler.EnsureUncompressed();
    DicomImageInfo info = handler.GetImageInfo();
    std::vector<uint8_t> pixels = handler.GetPixelData();

    uint16_t* samples = reinterpret_cast<uint16_t*>(pixels.data());

    auto setBlock = [&](uint32_t x0, int32_t storedValue) {
        for (uint32_t y = 0; y < kHu13BlockSize && y < info.height; ++y) {
            for (uint32_t x = x0; x < x0 + kHu13BlockSize && x < info.width; ++x) {
                samples[static_cast<size_t>(y) * info.width + x] =
                    static_cast<uint16_t>(static_cast<int16_t>(storedValue));
            }
        }
    };
    setBlock(kHu13AirX, kHu13AirStored);
    setBlock(kHu13BoneX, kHu13BoneStored);
    setBlock(kHu13PadX, kHu13PadStored);

    handler.SetNativePixelData(pixels);

    handler.SetUint16(0x0028, 0x0103, 1);   // PixelRepresentation -> signed
    handler.SetUint16(0x0028, 0x0101, 16);  // BitsStored
    handler.SetUint16(0x0028, 0x0102, 15);  // HighBit
    handler.SetSint16(0x0028, 0x0120, static_cast<int16_t>(kHu13PadStored));  // PixelPaddingValue

    return handler.WriteToBuffer(TS_LITTLE_ENDIAN_EXPLICIT);
}

// Comprehensive check of the HU13 lossy rewrite (transcode.cpp
// ApplyHu13DicomTags + pixel_layout.h ApplyHu13Rewrite): the TS/tag rewrite
// (BitsStored/HighBit/RescaleIntercept/Slope/PixelPadding*), and that values
// recovered through the plugin's own FROM-JXL decode path stay close to the
// source's - including that the 13-bit decode is UNSCALED (exercising
// JxlCodec::Decode's JxlDecoderSetImageOutBitDepth fix at the DICOM level;
// without it every code would come back multiplied by ~65535/8191, and the
// bone-block check below would fail hard).
static bool VerifyHu13Rewrite(const char* path, ThreadPool& pool) {
    bool allOk = true;
    auto dicom = ReadFile(path);

    {
        DicomHandler h(dicom.data(), dicom.size());
        std::string s;
        if (!h.GetString(0x0028, 0x1052, s)) {
            printf("hu13-rewrite  SKIPPED: %s has no RescaleIntercept\n\n", path);
            return true;
        }
    }

    auto rescaledDicom = MakeSyntheticHu13Fixture(dicom);

    DicomImageInfo info;
    double origIntercept = -1024.0, origSlope = 1.0;
    std::vector<uint8_t> origPixels;
    std::string origSopUid;
    {
        DicomHandler h(rescaledDicom.data(), rescaledDicom.size());
        info = h.GetImageInfo();
        origPixels = h.GetPixelData();
        h.GetString(0x0008, 0x0018, origSopUid);
        std::string s;
        if (h.GetString(0x0028, 0x1052, s)) origIntercept = std::atof(s.c_str());
        if (h.GetString(0x0028, 0x1053, s)) origSlope = std::atof(s.c_str());
    }

    const float kDistance = 0.5f;
    PluginConfig config = PluginConfig::Default();
    config.encodeOptions.mode = EncodeMode::ProgressiveVarDCT;
    config.encodeOptions.distance = kDistance;

    TranscodeResult lossy = TranscodeToJxl(rescaledDicom.data(), rescaledDicom.size(), config, pool);

    DicomHandler out(lossy.dicom.data(), lossy.dicom.size());
    const std::string ts = out.GetTransferSyntax();
    const DicomImageInfo outInfo = out.GetImageInfo();

    std::string imageType, sopUid, mediaSopUid, interceptStr, slopeStr;
    out.GetString(0x0008, 0x0008, imageType);
    out.GetString(0x0008, 0x0018, sopUid);
    out.GetMetaString(0x0002, 0x0003, mediaSopUid);
    out.GetString(0x0028, 0x1052, interceptStr);
    out.GetString(0x0028, 0x1053, slopeStr);

    uint16_t paddingValueOut = 0, paddingLimitOut = 0;
    const bool paddingValueReadable = out.GetUint16(0x0028, 0x0120, paddingValueOut);
    const bool paddingLimitReadable = out.GetUint16(0x0028, 0x0121, paddingLimitOut);

    struct Check { const char* name; bool ok; std::string detail; };
    std::vector<Check> checks;
    checks.push_back({"transfer syntax .112", ts == TS_JPEG_XL, ts});
    checks.push_back({"BitsStored=13", outInfo.bitsStored == 13, std::to_string(outInfo.bitsStored)});
    checks.push_back({"HighBit=12", outInfo.highBit == 12, std::to_string(outInfo.highBit)});
    checks.push_back({"PixelRepresentation unsigned", !outInfo.isSigned, ""});
    checks.push_back({"RescaleIntercept=-3136",
        std::fabs(std::atof(interceptStr.c_str()) - (-3136.0)) < 0.5, interceptStr});
    checks.push_back({"RescaleSlope=1", std::fabs(std::atof(slopeStr.c_str()) - 1.0) < 1e-9, slopeStr});
    checks.push_back({"PixelPaddingValue=0", paddingValueReadable && paddingValueOut == 0,
        std::to_string(paddingValueOut)});
    checks.push_back({"PixelPaddingRangeLimit=63", paddingLimitReadable && paddingLimitOut == 63,
        std::to_string(paddingLimitOut)});
    checks.push_back({"ImageType[0]=DERIVED",
        imageType.rfind("DERIVED", 0) == 0 &&
        (imageType.size() == 7 || imageType[7] == '\\'), imageType});
    checks.push_back({"SOPInstanceUID changed", !sopUid.empty() && sopUid != origSopUid, sopUid});
    checks.push_back({"MediaStorageSOPInstanceUID in sync", sopUid == mediaSopUid, mediaSopUid});

    for (const auto& c : checks) {
        if (!c.ok) allOk = false;
        printf("    %-38s %-20s %s\n", c.name, c.detail.c_str(), c.ok ? "PASS" : "FAIL");
    }

    // Decode the lossy bits back through the plugin's own FROM-JXL path.
    TranscodeResult fromLossy = TranscodeFromJxl(
        lossy.dicom.data(), lossy.dicom.size(), TS_LITTLE_ENDIAN_EXPLICIT, pool);
    DicomHandler decodedHandler(fromLossy.dicom.data(), fromLossy.dicom.size());
    std::vector<uint8_t> decodedPixels = decodedHandler.GetPixelData();

    const uint16_t* decCodes = reinterpret_cast<const uint16_t*>(decodedPixels.data());
    const uint16_t* origSamples = reinterpret_cast<const uint16_t*>(origPixels.data());
    const size_t n = std::min(decodedPixels.size(), origPixels.size()) / 2;
    const uint32_t width = info.width;

    auto inBlock = [&](uint32_t x, uint32_t y, uint32_t bx) {
        return y < kHu13BlockSize && x >= bx && x < bx + kHu13BlockSize;
    };
    auto inCore = [&](uint32_t x, uint32_t y, uint32_t bx) {
        return y < kHu13BlockSize - kHu13BlockMargin &&
               x >= bx + kHu13BlockMargin && x < bx + kHu13BlockSize - kHu13BlockMargin;
    };
    // The three blocks sit contiguously (with gaps) in one top-left strip -
    // exclude that whole strip, expanded by a margin, from the "natural
    // texture" RMSE/max-error measurement below. VarDCT ringing at a hard
    // synthetic edge bleeds into nearby pixels for a margin well beyond the
    // block itself; without this the measurement mixes real CT quality with
    // artefacts from blocks this test invented, not the image.
    constexpr uint32_t kHu13ExcludeMargin = 16;
    constexpr uint32_t kHu13ExcludeY = kHu13BlockSize + kHu13ExcludeMargin;
    constexpr uint32_t kHu13ExcludeXMax = kHu13PadX + kHu13BlockSize + kHu13ExcludeMargin;
    auto nearAnyBlock = [&](uint32_t x, uint32_t y) {
        return y < kHu13ExcludeY && x < kHu13ExcludeXMax;
    };

    bool paddingCoreOk = true;
    bool nonPaddingOk = true;    // every pixel outside the padding block must decode > 63
    bool boneCoreOk = true;
    bool boneCoreSeen = false;
    double sumSq = 0.0;
    double maxAbsErr = 0.0;
    size_t rmseCount = 0;

    for (size_t i = 0; i < n; ++i) {
        const uint32_t x = static_cast<uint32_t>(i % width);
        const uint32_t y = static_cast<uint32_t>(i / width);
        const uint16_t code = decCodes[i];
        const double huDecoded = static_cast<double>(code) - kHu13Offset;

        if (inBlock(x, y, kHu13PadX)) {
            if (inCore(x, y, kHu13PadX) && code > 63) paddingCoreOk = false;
        } else {
            if (code <= 63) nonPaddingOk = false;

            if (inCore(x, y, kHu13BoneX)) {
                boneCoreSeen = true;
                if (std::fabs(huDecoded - (kHu13BoneStored + origIntercept)) >= 50.0) {
                    boneCoreOk = false;
                }
            }

            // Natural-texture RMSE/max error, excluding the synthetic blocks
            // (and their ringing margin) entirely - this is what the
            // "d=0.5 -> RMSE 5.9" measurement in the README/config.h comment
            // describes.
            if (!nearAnyBlock(x, y)) {
                const double huOrig =
                    static_cast<double>(ExtractStoredValue<uint16_t>(origSamples[i], 16, /*isSigned=*/true))
                        * origSlope + origIntercept;
                const double diff = huDecoded - huOrig;
                sumSq += diff * diff;
                maxAbsErr = std::max(maxAbsErr, std::fabs(diff));
                ++rmseCount;
            }
        }
    }
    if (!boneCoreSeen) boneCoreOk = false;

    const double rmse = rmseCount ? std::sqrt(sumSq / static_cast<double>(rmseCount)) : 0.0;
    const bool rmseOk = rmse < 10.0 && maxAbsErr < 100.0;

    printf("    %-38s %-20s %s\n", "padding block core decodes <= 63", "", paddingCoreOk ? "PASS" : "FAIL");
    printf("    %-38s %-20s %s\n", "every non-padding pixel decodes > 63", "", nonPaddingOk ? "PASS" : "FAIL");
    printf("    %-38s RMSE=%-8.2f maxAbsErr=%-8.2f %s\n", "non-padding HU vs. original", rmse, maxAbsErr,
           rmseOk ? "PASS" : "FAIL");
    printf("    %-38s %-20s %s\n", "bone block unscaled (~2000 HU, not x8)", "", boneCoreOk ? "PASS" : "FAIL");

    if (!paddingCoreOk) allOk = false;
    if (!nonPaddingOk) allOk = false;
    if (!rmseOk) allOk = false;
    if (!boneCoreOk) allOk = false;

    printf("%-40s d=%.1f -> %s  %s\n\n", path, kDistance, ts.c_str(), allOk ? "PASS" : "FAIL");
    return allOk;
}

// --- JxlCodec nominal bit depth (codec-level, no DICOM involved) -----------

// Unit test for JxlCodec's nominal-bit-depth encode/decode path in isolation
// from any DICOM tag rewrite (jxl_codec.cpp: EncodeOptions::nominalBits via
// JxlEncoderSetFrameBitDepth on encode, JxlDecoderSetImageOutBitDepth on
// decode). Encodes a small synthetic 13-bit-nominal Gray16 image (raw codes
// 0..8191, unscaled - JxlEncoderSetFrameBitDepth tells libjxl the buffer is
// already in that range) and decodes it back, checking the codes come back
// unscaled rather than the ~8x (65535/8191) a decoder without the bit-depth
// fix would produce.
static bool TestNominalBitDepthCodec() {
    bool allOk = true;
    constexpr uint32_t kWidth = 64, kHeight = 64;
    constexpr uint32_t kNominalBits = 13;
    constexpr uint16_t kMaxCode = (1u << kNominalBits) - 1;  // 8191

    // Smooth horizontal gradient spanning the full 13-bit range - easy for a
    // lossy VarDCT encode to preserve closely, so a large error only shows up
    // if the bit-depth handling is actually wrong.
    std::vector<uint16_t> codes(static_cast<size_t>(kWidth) * kHeight);
    for (uint32_t y = 0; y < kHeight; ++y) {
        for (uint32_t x = 0; x < kWidth; ++x) {
            codes[y * kWidth + x] =
                static_cast<uint16_t>((static_cast<uint64_t>(x) * kMaxCode) / (kWidth - 1));
        }
    }

    {
        EncodeOptions opts = EncodeOptions::Lossless(7);
        opts.nominalBits = kNominalBits;
        auto encoded = JxlCodec::Encode(codes.data(), kWidth, kHeight, PixelFormat::Gray16, opts);
        auto decoded = JxlCodec::Decode(encoded, PixelFormat::Gray16);
        const uint16_t* decCodes = reinterpret_cast<const uint16_t*>(decoded.data());
        const bool exact = decoded.size() == codes.size() * 2 &&
                           std::equal(codes.begin(), codes.end(), decCodes);
        printf("    %-38s %-20s %s\n", "nominalBits=13 lossless exact", "", exact ? "PASS" : "FAIL");
        if (!exact) allOk = false;
    }

    {
        EncodeOptions opts = EncodeOptions::ProgressiveVarDCT(7, 0.5f);
        opts.nominalBits = kNominalBits;
        auto encoded = JxlCodec::Encode(codes.data(), kWidth, kHeight, PixelFormat::Gray16, opts);
        auto decoded = JxlCodec::Decode(encoded, PixelFormat::Gray16);
        const uint16_t* decCodes = reinterpret_cast<const uint16_t*>(decoded.data());
        const size_t n = decoded.size() / 2;

        uint16_t maxDecoded = 0;
        double sumSq = 0.0;
        for (size_t i = 0; i < n; ++i) {
            maxDecoded = std::max(maxDecoded, decCodes[i]);
            const double diff = static_cast<double>(decCodes[i]) - static_cast<double>(codes[i]);
            sumSq += diff * diff;
        }
        const double rmse = n ? std::sqrt(sumSq / static_cast<double>(n)) : 0.0;

        // Unscaled: every code must stay within (a little headroom over) the
        // declared 13-bit range. A decoder without the bit-depth fix would
        // scale by ~65535/8191 (~8x), pushing values into the tens of
        // thousands - nowhere close to this bound.
        const bool unscaled = maxDecoded <= kMaxCode + 64;
        const bool closeEnough = rmse < 100.0;
        printf("    %-38s max=%-6u rmse=%-8.2f %s\n", "nominalBits=13 lossy unscaled+close",
               maxDecoded, rmse, (unscaled && closeEnough) ? "PASS" : "FAIL");
        if (!unscaled || !closeEnough) allOk = false;
    }

    printf("nominal-bit-depth (codec-level)          %s\n\n", allOk ? "PASS" : "FAIL");
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

    // HU13 (rescaled) lossy rewrite check (uses the first input file).
    try {
        if (!VerifyHu13Rewrite(argv[1], pool)) {
            ++failures;
        }
    } catch (const std::exception& e) {
        printf("hu13-rewrite  ERROR: %s\n", e.what());
        ++failures;
    }

    // Codec-level nominal-bit-depth check (no DICOM fixture needed).
    try {
        if (!TestNominalBitDepthCodec()) {
            ++failures;
        }
    } catch (const std::exception& e) {
        printf("nominal-bit-depth  ERROR: %s\n", e.what());
        ++failures;
    }

    printf("\n%s\n", failures == 0 ? "ALL PASSED" : "FAILURES PRESENT");
    return failures == 0 ? 0 : 1;
}
