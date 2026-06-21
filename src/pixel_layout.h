/*
 * Copyright (C) 2026 Ryan Walklin <ryan@kaitakeradiology.co.nz>
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

#include <cstdint>
#include <cstring>
#include <vector>

namespace orthanc_jxl {

/**
 * Convert one planar frame (R..R G..G B..B) to interleaved (R G B R G B ...).
 *
 * DICOM encapsulated/compressed pixel data must be colour-by-pixel
 * (PlanarConfiguration 0), so planar uncompressed input is converted before
 * encoding. The transform is byte-exact and reversible.
 */
inline std::vector<uint8_t> PlanarToInterleaved(const uint8_t* src,
                                                size_t pixelsPerFrame,
                                                int numChannels,
                                                int bytesPerSample) {
    std::vector<uint8_t> out(pixelsPerFrame * numChannels * bytesPerSample);
    const size_t plane = pixelsPerFrame * bytesPerSample;
    for (int c = 0; c < numChannels; ++c) {
        const uint8_t* planeSrc = src + static_cast<size_t>(c) * plane;
        for (size_t p = 0; p < pixelsPerFrame; ++p) {
            std::memcpy(out.data() + (p * numChannels + c) * bytesPerSample,
                        planeSrc + p * bytesPerSample,
                        bytesPerSample);
        }
    }
    return out;
}

}  // namespace orthanc_jxl
