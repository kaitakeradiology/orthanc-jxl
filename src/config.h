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
 *
 * Mode=ProgressiveVarDCT with Distance>0 is a LOSSY encode, always written
 * under transfer syntax 1.2.840.10008.1.2.4.112 (JPEG XL), never .110 - see
 * transfer_syntax.h and plugin.cpp's transcoder callback for how the
 * requested syntax list interacts with this. To make the lossy distance
 * budget count, the encode enables libjxl's XYB colour transform
 * (uses_original_profile=FALSE, sRGB transfer function - see jxl_codec.cpp)
 * instead of quantising in the raw stored-unit space, and - for signed
 * pixel data (PixelRepresentation=1) - biases every sample by
 * 2^(BitsStored-1) into unsigned "offset binary" before encoding, undone on
 * decode via an adjusted RescaleIntercept (see dicom_handler.cpp/
 * transcode.cpp ApplyLossyTags). The lossy rewrite also sets the PS3.3
 * C.7.6.1.1.5 tags (LossyImageCompression/Ratio/Method), marks ImageType
 * DERIVED, and assigns a new SOPInstanceUID.
 *
 * Measured on one 512x512 signed CT (-2000..3622, BitsStored=16), sRGB
 * transfer function + signed offset, RMSE in stored (HU) units:
 *   Distance  Size    RMSE
 *   0.25      ~10KB    31
 *   0.5        7KB     41
 *   1.0        5KB     76
 * (Without the offset/XYB fix, d=1.0 was 19.9KB at RMSE~147 - about a third
 * of a soft-tissue window - because uses_original_profile=TRUE disabled XYB
 * and the sign discontinuity ate the distance budget.)
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

    // Set by Parse() when ProgressiveDC/ProgressiveAC/CenterFirstOrdering
    // would hit the libjxl 0.12 PROGRESSIVE_DC+AC-with-explicit-centre defect
    // (see jxl_codec.cpp, ProgressiveVarDCT case) on every VarDCT encode.
    // jxl_codec.cpp clamps PROGRESSIVE_AC off unconditionally regardless of
    // this flag - it exists purely so plugin.cpp can log the situation once
    // at startup instead of the codec logging (or not logging) it per image.
    bool progressiveAcDefectWillBeClamped = false;

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
