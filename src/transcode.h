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

#pragma once

#include "config.h"
#include "thread_pool.h"

#include <cstdint>
#include <string>
#include <vector>

namespace orthanc_jxl {

// Result of a transcode: the serialized DICOM plus stats for logging.
struct TranscodeResult {
    std::vector<uint8_t> dicom;
    uint32_t frameCount = 0;
    size_t nativeBytes = 0;    // total uncompressed pixel bytes
    size_t encodedBytes = 0;   // total JXL pixel bytes (TO-JXL only)
};

// Encode an uncompressed/legacy DICOM instance to JPEG-XL. Frames are encoded
// in parallel on the supplied pool. Throws on malformed input.
//
// singleFrameThreads controls libjxl's internal worker count for single-frame
// instances: < 0 uses libjxl's default (one per core), 1 forces single-threaded
// so that ingest-level concurrency, not nested threads, fills the cores.
// Multi-frame instances always encode one (single-threaded) frame per pool
// worker regardless of this value.
TranscodeResult TranscodeToJxl(const void* dicom, size_t size,
                               const PluginConfig& config, ThreadPool& pool,
                               int singleFrameThreads = -1);

// Decode a JPEG-XL DICOM instance back to the given uncompressed transfer
// syntax. Frames are decoded in parallel on the supplied pool.
TranscodeResult TranscodeFromJxl(const void* dicom, size_t size,
                                 const std::string& uncompressedTs, ThreadPool& pool);

}  // namespace orthanc_jxl
