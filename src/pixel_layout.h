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
#include <type_traits>
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

/**
 * Bias one signed (two's-complement, BitsStored bits) sample into unsigned
 * "offset binary" by adding 2^(BitsStored-1), leaving any padding bits above
 * BitsStored (which DICOM PS3.5 requires to be 0) untouched.
 *
 * Two's-complement samples put the padding region (all-ones, e.g. 63536+ for
 * a 16-bit-stored value) immediately next to small positive values - a
 * ~60000-unit cliff a perceptual (VarDCT) codec has no way to represent
 * cheaply and ends up smearing across real tissue values instead. Adding a
 * constant removes the cliff entirely; RescaleIntercept is adjusted by the
 * same constant so the recovered HU values are unchanged (see
 * ApplyLossyTags in transcode.cpp). Only meaningful for a lossy encode -
 * lossless modes must stay bit-exact and never call this.
 *
 * Generic over the sample width (T = uint8_t or uint16_t) because the
 * BitsStored-bit two's complement value is NOT necessarily a plain
 * sizeof(T)*8-bit two's complement value when BitsStored < sizeof(T)*8 (e.g.
 * 12-bit-stored CT in a 16-bit word) - the sign bit lives at BitsStored-1,
 * not at the top of the word.
 */
template <typename T>
inline T SignedSampleToOffsetBinary(T raw, uint16_t bitsStored) {
    static_assert(std::is_unsigned<T>::value, "T must be an unsigned integer type");
    constexpr uint32_t kWordBits = sizeof(T) * 8;
    const uint32_t mask = (bitsStored >= kWordBits)
        ? static_cast<uint32_t>(static_cast<T>(~T(0)))
        : ((1u << bitsStored) - 1u);
    const uint32_t signBit = 1u << (bitsStored - 1);
    const uint32_t stored = static_cast<uint32_t>(raw) & mask;
    const int32_t signedVal = (stored & signBit)
        ? static_cast<int32_t>(stored) - static_cast<int32_t>(mask) - 1
        : static_cast<int32_t>(stored);
    const uint32_t unsignedVal =
        static_cast<uint32_t>(signedVal + static_cast<int32_t>(signBit)) & mask;
    return static_cast<T>((static_cast<uint32_t>(raw) & ~mask) | unsignedVal);
}

// Apply SignedSampleToOffsetBinary in place across every sample in a buffer
// (all frames concatenated - the transform is per-sample, frame boundaries
// don't matter).
template <typename T>
inline void ApplySignedOffset(T* samples, size_t sampleCount, uint16_t bitsStored) {
    for (size_t i = 0; i < sampleCount; ++i) {
        samples[i] = SignedSampleToOffsetBinary<T>(samples[i], bitsStored);
    }
}

}  // namespace orthanc_jxl
