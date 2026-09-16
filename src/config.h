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
 *     "ProgressiveDC": 1,              // VarDCT only: 0-2 (1 = DC frame first, the streaming L0)
 *     "ProgressiveAC": true,           // VarDCT only: coarse-to-fine AC passes
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
 * instead of quantising in the raw stored-unit space, and declares the
 * source's real (nominal) bit depth to the encoder instead of the full
 * 16-bit container Gray16/RGB48 implies (EncodeOptions::nominalBits) - a
 * full 16-bit declaration for e.g. 12-bit CT makes the encoder's perceptual
 * distance model see the real signal as a tiny fraction of full scale.
 *
 * For a dataset with RescaleIntercept (almost always CT/PET/MR), the pixel
 * data is additionally re-quantised directly into a 13-bit HU codeword
 * (code = round(HU) + 3136, valid 1..8191; code 0 reserved for pixel
 * padding) rather than encoding raw stored units - see the kHu13* constants
 * in pixel_layout.h for the full rationale and transcode.cpp
 * ApplyHu13DicomTags for the resulting tag rewrite (BitsStored 13,
 * RescaleIntercept -3136, RescaleSlope 1, PixelPaddingValue/RangeLimit 0/63
 * when padding was declared). A dataset WITHOUT RescaleIntercept instead
 * keeps the older scheme: for signed pixel data (PixelRepresentation=1),
 * biases every sample by 2^(BitsStored-1) into unsigned "offset binary"
 * before encoding, undone on decode via an adjusted RescaleIntercept (see
 * dicom_handler.cpp/transcode.cpp ApplyOffsetSchemeTags). Either way the
 * lossy rewrite also sets the PS3.3 C.7.6.1.1.5 tags (LossyImageCompression/
 * Ratio/Method), marks ImageType DERIVED, and assigns a new SOPInstanceUID
 * (ApplyCommonLossyTags).
 *
 * Measured on one GE head CT slice at a brain window (jxl-rs decode), HU13
 * codeword layout, XYB + sRGB transfer function:
 *   Distance  Size    RMSE (brain interior, HU)
 *   0.2       26KB     4.0
 *   0.5       16.7KB   5.9  (p99 16)
 *   1.0       12.7KB   7.2
 *   lossless  120KB    0
 * Without declaring the real depth, the same brain window was ~0.1% of full
 * scale to the perceptual model: d=0.1 -> 12.8KB at RMSE 9.2, with libjxl's
 * distance floor capping achievable quality near 8 HU regardless of how far
 * d dropped further.
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
