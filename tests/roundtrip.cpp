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

#include <cstdio>
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

static bool RunOne(const char* path, ThreadPool& pool) {
    auto dicom = ReadFile(path);

    DicomImageInfo info;
    std::vector<uint8_t> origPixels;
    {
        DicomHandler handler(dicom.data(), dicom.size());
        info = handler.GetImageInfo();
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

    bool tsOk = IsJxlTransferSyntax(jxlTs);
    bool framesOk = (encFrames == info.numberOfFrames) &&
                    (fromJxl.frameCount == info.numberOfFrames);
    bool sizeOk = (rtPixels.size() == expected.size());
    bool bytesOk = sizeOk && (rtPixels == expected);
    bool pass = tsOk && framesOk && bytesOk;

    double ratio = toJxl.encodedBytes
        ? static_cast<double>(toJxl.nativeBytes) / toJxl.encodedBytes : 0.0;

    printf("%-40s %3ux%-3u f=%-3u spp=%u ba=%-2u %-14s planar=%u  %5.2fx  %s\n",
           path, info.width, info.height, info.numberOfFrames,
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

// Verify a lossy (distance > 0) configuration is labelled with the lossy JXL
// transfer syntax (.112), not mathematically-lossless (.110).
static bool VerifyLossyLabeling(const char* path, ThreadPool& pool) {
    auto dicom = ReadFile(path);
    PluginConfig config = PluginConfig::Default();
    config.encodeOptions.mode = EncodeMode::ProgressiveVarDCT;
    config.encodeOptions.distance = 1.0f;

    TranscodeResult r = TranscodeToJxl(dicom.data(), dicom.size(), config, pool);
    DicomHandler h(r.dicom.data(), r.dicom.size());
    std::string ts = h.GetTransferSyntax();
    bool ok = (ts == TS_JPEG_XL);
    printf("%-40s lossy distance=1.0 -> %s  %s\n", path, ts.c_str(),
           ok ? "PASS" : "FAIL (expected .112)");
    return ok;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <dicom_file> [<dicom_file> ...]\n", argv[0]);
        return 2;
    }

    unsigned hw = std::thread::hardware_concurrency();
    ThreadPool pool(hw == 0 ? 1u : hw);

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

    // Lossy transfer-syntax labelling check (uses the first input file).
    printf("\n");
    try {
        if (!VerifyLossyLabeling(argv[1], pool)) {
            ++failures;
        }
    } catch (const std::exception& e) {
        printf("lossy-labeling  ERROR: %s\n", e.what());
        ++failures;
    }

    printf("\n%s\n", failures == 0 ? "ALL PASSED" : "FAILURES PRESENT");
    return failures == 0 ? 0 : 1;
}
