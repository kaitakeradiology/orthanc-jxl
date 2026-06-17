/*
 * Copyright (C) 2025 Ryan Walklin <ryan@kaitakeradiology.co.nz>
 *
 * This file is part of orthanc-jxl.
 *
 * orthanc-jxl is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * orthanc-jxl is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * orthanc-jxl. If not, see <https://www.gnu.org/licenses/>.
 */

#include "transcode.h"

#include "dicom_handler.h"
#include "jxl_codec.h"
#include "pixel_layout.h"
#include "transfer_syntax.h"

namespace orthanc_jxl {

namespace {

// Pick the JXL PixelFormat for the given DICOM geometry.
PixelFormat PixelFormatFor(const DicomImageInfo& info) {
    if (info.samplesPerPixel == 1) {
        return (info.bitsAllocated <= 8) ? PixelFormat::Gray8 : PixelFormat::Gray16;
    }
    return (info.bitsAllocated <= 8) ? PixelFormat::RGB24 : PixelFormat::RGB48;
}

}  // namespace

TranscodeResult TranscodeToJxl(const void* dicom, size_t size,
                               const PluginConfig& config, ThreadPool& pool,
                               int singleFrameThreads) {
    DicomHandler handler(dicom, size);

    DicomImageInfo info = handler.GetImageInfo();

    const size_t frameSize = info.FrameSizeBytes();
    const uint32_t frameCount = info.numberOfFrames;
    if (frameSize == 0 || frameCount == 0) {
        throw DicomHandlerError("Invalid image geometry for encoding");
    }

    // Validate the pixel buffer actually holds every frame before slicing it -
    // guards against malformed/truncated instances.
    std::vector<uint8_t> pixels = handler.GetPixelData();
    const size_t expected = static_cast<size_t>(frameCount) * frameSize;
    if (pixels.size() < expected) {
        throw DicomHandlerError("Pixel data smaller than declared geometry");
    }

    const PixelFormat format = PixelFormatFor(info);
    const int channels = JxlCodec::NumChannels(format);
    const int bytesPerSample = JxlCodec::BitsPerSample(format) / 8;
    const bool planar = (info.planarConfiguration == 1 && info.samplesPerPixel > 1);

    EncodeOptions opts = config.GetEncodeOptions(info.width, info.height);
    const bool lossy =
        (opts.mode == EncodeMode::ProgressiveVarDCT && opts.distance > 0.0f);
    const std::string outTs = lossy ? TS_JPEG_XL : TS_JPEG_XL_LOSSLESS;

    // Frame-parallel: encode each frame single-threaded so the shared pool, not
    // nested libjxl runners, is the sole source of threads. A lone frame falls
    // back to libjxl's own threads (overridable for ingest-concurrency tuning).
    const int frameThreads = (frameCount > 1)
        ? JxlCodec::kSingleThreaded : singleFrameThreads;

    std::vector<std::vector<uint8_t>> encoded(frameCount);
    ParallelFor(pool, frameCount, [&](size_t f) {
        const uint8_t* src = pixels.data() + f * frameSize;
        if (planar) {
            std::vector<uint8_t> interleaved = PlanarToInterleaved(
                src, static_cast<size_t>(info.width) * info.height,
                channels, bytesPerSample);
            encoded[f] = JxlCodec::Encode(interleaved.data(), info.width,
                                          info.height, format, opts, frameThreads);
        } else {
            encoded[f] = JxlCodec::Encode(src, info.width, info.height,
                                          format, opts, frameThreads);
        }
    });

    handler.SetEncapsulatedFrames(encoded, outTs);
    if (planar) {
        // Encapsulated pixel data is colour-by-pixel by definition.
        handler.SetUint16(0x0028, 0x0006, 0);  // PlanarConfiguration
    }
    handler.SetTransferSyntax(outTs);

    TranscodeResult result;
    result.dicom = handler.WriteToBuffer(outTs);
    result.frameCount = frameCount;
    result.nativeBytes = expected;
    result.encodedBytes = 0;
    for (const auto& e : encoded) {
        result.encodedBytes += e.size();
    }
    return result;
}

TranscodeResult TranscodeFromJxl(const void* dicom, size_t size,
                                 const std::string& uncompressedTs, ThreadPool& pool) {
    DicomHandler handler(dicom, size);

    uint32_t frameCount = handler.GetEncapsulatedFrameCount();
    if (frameCount == 0) {
        throw DicomHandlerError("JXL pixel data has no frames");
    }

    // Pull every encapsulated frame up front (DCMTK access is not thread-safe),
    // then decode frames in parallel.
    std::vector<std::vector<uint8_t>> jxlFrames(frameCount);
    for (uint32_t f = 0; f < frameCount; ++f) {
        jxlFrames[f] = handler.GetEncapsulatedData(f);
    }

    std::vector<std::vector<uint8_t>> decoded(frameCount);
    const int frameThreads = (frameCount > 1)
        ? JxlCodec::kSingleThreaded : JxlCodec::kDefaultThreads;
    ParallelFor(pool, frameCount, [&](size_t f) {
        decoded[f] = JxlCodec::Decode(jxlFrames[f], frameThreads).first;
    });

    // Concatenate frames into a single native pixel-data blob.
    size_t totalSize = 0;
    for (const auto& d : decoded) {
        totalSize += d.size();
    }
    std::vector<uint8_t> pixels;
    pixels.reserve(totalSize);
    for (auto& d : decoded) {
        pixels.insert(pixels.end(), d.begin(), d.end());
    }

    handler.SetNativePixelData(pixels);
    handler.SetTransferSyntax(uncompressedTs);

    TranscodeResult result;
    result.dicom = handler.WriteToBuffer(uncompressedTs);
    result.frameCount = frameCount;
    result.nativeBytes = pixels.size();
    result.encodedBytes = 0;
    return result;
}

}  // namespace orthanc_jxl
