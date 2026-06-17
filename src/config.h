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

#include "jxl_codec.h"
#include <string>

namespace orthanc_jxl {

/**
 * Plugin configuration parsed from Orthanc config file.
 *
 * Example configuration in orthanc.json:
 * {
 *   "OrthancJxl": {
 *     "Mode": "ProgressiveLossless",  // "Lossless", "ProgressiveLossless", "ProgressiveVarDCT"
 *     "Effort": 7,                     // 1-10
 *     "Distance": 0.0,                 // 0.0 = lossless, >0 for lossy
 *     "CenterFirstOrdering": true,     // Enable center-first group ordering
 *     "ProgressiveDC": 0,              // VarDCT only: 0-2
 *     "ProgressiveAC": false,          // VarDCT only
 *     "EncodeThreads": 0               // Single-frame encode threads: 0=auto, 1=single
 *   }
 * }
 */
struct PluginConfig {
    EncodeOptions encodeOptions;
    bool centerFirstOrdering = true;  // Use image center for group ordering

    // libjxl worker threads per single-frame encode:
    //   0 = auto (one per core) - best when instances are ingested sequentially
    //   1 = single-threaded     - best when many instances transcode in parallel
    //                             (concurrent DICOMweb pull / STOW), avoids
    //                             oversubscribing cores
    //   N = that many threads
    // Multi-frame instances are unaffected (always one single-threaded frame per
    // pool worker).
    int encodeThreads = 0;

    // Resolve encodeThreads into the codec's worker-thread convention
    // (0 -> -1 = libjxl default).
    int SingleFrameThreads() const { return encodeThreads == 0 ? -1 : encodeThreads; }

    // Get encode options with center coordinates applied
    EncodeOptions GetEncodeOptions(uint32_t imageWidth, uint32_t imageHeight) const;

    // Default configuration
    static PluginConfig Default();

    // Parse from Orthanc JSON config string
    // Returns default config if parsing fails or section is missing
    static PluginConfig Parse(const char* jsonConfig);
};

} // namespace orthanc_jxl
