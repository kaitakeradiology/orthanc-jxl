/*
 * Throughput benchmark simulating a study import.
 *
 * Replicates an input DICOM instance N times and transcodes all copies TO JXL
 * across a sweep of ingest-concurrency levels (worker threads each calling the
 * real transcode path). Reports wall time and instances/sec so we can see how
 * import speed scales and pick an ingest concurrency.
 *
 * Usage: throughput <dicom_file> [copies]
 */

#include "../src/transcode.h"
#include "../src/dicom_handler.h"
#include "../src/config.h"
#include "../src/thread_pool.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <thread>
#include <vector>

using namespace orthanc_jxl;

static std::vector<uint8_t> ReadFile(const char* path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error(std::string("Failed to open: ") + path);
    size_t size = file.tellg();
    file.seekg(0);
    std::vector<uint8_t> data(size);
    file.read(reinterpret_cast<char*>(data.data()), size);
    return data;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <dicom_file> [copies]\n", argv[0]);
        return 2;
    }
    const int copies = (argc >= 3) ? std::atoi(argv[2]) : 200;

    auto dicom = ReadFile(argv[1]);
    DicomImageInfo info;
    {
        DicomHandler h(dicom.data(), dicom.size());
        info = h.GetImageInfo();
    }

    const unsigned hw = std::thread::hardware_concurrency();
    ThreadPool pool(hw == 0 ? 1u : hw);
    PluginConfig config = PluginConfig::Default();

    const double mbIn = (info.FrameSizeBytes() * (double)info.numberOfFrames) / (1024.0 * 1024.0);
    printf("Input: %ux%u f=%u spp=%u ba=%u  (%.2f MB native/instance)\n",
           info.width, info.height, info.numberOfFrames,
           info.samplesPerPixel, info.bitsAllocated, mbIn);
    printf("Transcoding %d copies TO JXL (%s), hw_threads=%u\n\n",
           copies, "ProgressiveLossless e7", hw);

    std::vector<unsigned> sweep = {1, 2, 4};
    if (hw > 4) sweep.push_back(hw);

    // Two strategies for single-frame encodes:
    //   "libjxl-threads": each encode uses libjxl's default worker count
    //   "1-thread/encode": each encode is single-threaded; cores filled by ingest
    struct Strategy { const char* name; int threads; };
    const Strategy strategies[] = {
        {"libjxl-threads", -1},
        {"1-thread/encode", 1},
    };

    for (const auto& strat : strategies) {
        printf("== single-frame encode strategy: %s ==\n", strat.name);
        printf("%-9s %12s %14s %12s\n", "Ingest", "Wall (s)", "Instances/s", "MB native/s");
        for (unsigned workers : sweep) {
            std::atomic<int> next{0};
            std::atomic<int> errors{0};
            auto work = [&]() {
                for (;;) {
                    int i = next.fetch_add(1);
                    if (i >= copies) break;
                    try {
                        auto r = TranscodeToJxl(dicom.data(), dicom.size(), config,
                                                pool, strat.threads);
                        (void)r;
                    } catch (...) {
                        errors.fetch_add(1);
                    }
                }
            };

            auto t0 = std::chrono::high_resolution_clock::now();
            std::vector<std::thread> threads;
            for (unsigned w = 0; w < workers; ++w) threads.emplace_back(work);
            for (auto& t : threads) t.join();
            auto t1 = std::chrono::high_resolution_clock::now();

            double secs = std::chrono::duration<double>(t1 - t0).count();
            printf("%-9u %12.3f %14.1f %12.1f%s\n",
                   workers, secs, copies / secs, (copies * mbIn) / secs,
                   errors.load() ? "  (ERRORS!)" : "");
        }
        printf("\n");
    }
    return 0;
}
