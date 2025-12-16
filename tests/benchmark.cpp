/*
 * Benchmark and verification tool for JPEG-XL encoder
 *
 * Tests encoding modes and effort levels, verifies lossless roundtrip.
 * Usage: benchmark <dicom_file>
 */

#include "../src/jxl_codec.h"
#include "../src/dicom_handler.h"
#include <chrono>
#include <cstdio>
#include <fstream>
#include <cstring>

using namespace orthanc_jxl;

std::vector<uint8_t> ReadFile(const char* path) {
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

struct BenchResult {
    const char* mode;
    int effort;
    double encodeMs;
    double decodeMs;
    size_t outputSize;
    size_t inputSize;
    bool verified;
    bool isLossy;
};

void PrintResults(const std::vector<BenchResult>& results, const DicomImageInfo& info) {
    printf("\nImage: %ux%u, %u-bit, %u samples/pixel\n",
           info.width, info.height, info.bitsStored, info.samplesPerPixel);
    printf("Raw size: %.2f KB\n\n", results[0].inputSize / 1024.0);

    printf("%-24s %6s %10s %10s %10s %8s %10s\n",
           "Mode", "Effort", "Enc (ms)", "Dec (ms)", "Size (KB)", "Ratio", "Roundtrip");
    printf("%-24s %6s %10s %10s %10s %8s %10s\n",
           "------------------------", "------", "----------", "----------",
           "----------", "--------", "----------");

    for (const auto& r : results) {
        double ratio = static_cast<double>(r.inputSize) / r.outputSize;
        const char* rtStatus = r.isLossy ? "N/A" : (r.verified ? "yes" : "FAIL");
        printf("%-24s %6d %10.1f %10.1f %10.1f %7.2fx %10s\n",
               r.mode, r.effort, r.encodeMs, r.decodeMs,
               r.outputSize / 1024.0, ratio, rtStatus);
    }
    printf("\n");
}

BenchResult RunTest(const char* modeName, EncodeMode mode, int effort,
                    const void* pixels, size_t pixelSize,
                    uint32_t width, uint32_t height, PixelFormat format,
                    bool verifyRoundtrip) {
    BenchResult result{};
    result.mode = modeName;
    result.effort = effort;
    result.inputSize = pixelSize;

    EncodeOptions opts;
    opts.mode = mode;
    opts.effort = effort;
    opts.centerX = -1;
    opts.centerY = -1;

    // Encode
    auto encStart = std::chrono::high_resolution_clock::now();
    auto encoded = JxlCodec::Encode(pixels, width, height, format, opts);
    auto encEnd = std::chrono::high_resolution_clock::now();

    result.outputSize = encoded.size();
    result.encodeMs = std::chrono::duration<double, std::milli>(encEnd - encStart).count();

    // Decode
    auto decStart = std::chrono::high_resolution_clock::now();
    auto decoded = JxlCodec::Decode(encoded, format);
    auto decEnd = std::chrono::high_resolution_clock::now();

    result.decodeMs = std::chrono::duration<double, std::milli>(decEnd - decStart).count();

    // Verify lossless roundtrip
    result.isLossy = !verifyRoundtrip;
    if (verifyRoundtrip) {
        result.verified = (decoded.size() == pixelSize) &&
                          (std::memcmp(decoded.data(), pixels, pixelSize) == 0);
    } else {
        result.verified = false;
    }

    return result;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <dicom_file>\n", argv[0]);
        return 1;
    }

    try {
        printf("Loading DICOM: %s\n", argv[1]);
        auto dicomData = ReadFile(argv[1]);

        DicomHandler handler(dicomData.data(), dicomData.size());
        auto info = handler.GetImageInfo();
        auto pixels = handler.GetPixelData();

        // Determine pixel format
        PixelFormat format;
        if (info.samplesPerPixel == 1) {
            format = (info.bitsAllocated <= 8) ? PixelFormat::Gray8 : PixelFormat::Gray16;
        } else {
            format = (info.bitsAllocated <= 8) ? PixelFormat::RGB24 : PixelFormat::RGB48;
        }

        std::vector<BenchResult> results;

        printf("\nRunning benchmarks...\n");

        // Progressive Lossless (default mode) - effort 7 and 9
        for (int effort : {7, 9}) {
            printf("  ProgressiveLossless e%d...\n", effort);
            results.push_back(RunTest(
                "ProgressiveLossless", EncodeMode::ProgressiveLossless, effort,
                pixels.data(), pixels.size(), info.width, info.height, format, true));
        }

        // Non-progressive Lossless - effort 7 and 9
        for (int effort : {7, 9}) {
            printf("  Lossless e%d...\n", effort);
            results.push_back(RunTest(
                "Lossless", EncodeMode::Lossless, effort,
                pixels.data(), pixels.size(), info.width, info.height, format, true));
        }

        // Lossy VarDCT (distance 1.0) - effort 7 and 9
        for (int effort : {7, 9}) {
            printf("  Lossy (d=1.0) e%d...\n", effort);
            EncodeOptions opts;
            opts.mode = EncodeMode::ProgressiveVarDCT;
            opts.effort = effort;
            opts.distance = 1.0f;

            auto encStart = std::chrono::high_resolution_clock::now();
            auto encoded = JxlCodec::Encode(pixels.data(), info.width, info.height, format, opts);
            auto encEnd = std::chrono::high_resolution_clock::now();

            auto decStart = std::chrono::high_resolution_clock::now();
            auto decoded = JxlCodec::Decode(encoded, format);
            auto decEnd = std::chrono::high_resolution_clock::now();

            BenchResult r{};
            r.mode = "Lossy (d=1.0)";
            r.effort = effort;
            r.inputSize = pixels.size();
            r.outputSize = encoded.size();
            r.encodeMs = std::chrono::duration<double, std::milli>(encEnd - encStart).count();
            r.decodeMs = std::chrono::duration<double, std::milli>(decEnd - decStart).count();
            r.verified = false;
            r.isLossy = true;
            results.push_back(r);
        }

        PrintResults(results, info);

        // Summary - only check lossless modes
        bool allPassed = true;
        for (const auto& r : results) {
            if (!r.isLossy && !r.verified) {
                allPassed = false;
                fprintf(stderr, "FAIL: %s e%d - roundtrip verification failed\n",
                        r.mode, r.effort);
            }
        }

        return allPassed ? 0 : 1;

    } catch (const std::exception& e) {
        fprintf(stderr, "Error: %s\n", e.what());
        return 1;
    }
}
