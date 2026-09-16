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

#include <algorithm>
#include <cmath>
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
 * ApplyOffsetSchemeTags in transcode.cpp). Only meaningful for a lossy encode -
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

// Read one raw BitsStored-bit sample (packed in a wider word, same masking
// rule as SignedSampleToOffsetBinary above - the sign bit lives at
// BitsStored-1, not the top of the word) as its actual signed integer value.
// isSigned selects two's-complement vs. plain unsigned interpretation.
template <typename T>
inline int32_t ExtractStoredValue(T raw, uint16_t bitsStored, bool isSigned) {
    static_assert(std::is_unsigned<T>::value, "T must be an unsigned integer type");
    constexpr uint32_t kWordBits = sizeof(T) * 8;
    const uint32_t mask = (bitsStored >= kWordBits)
        ? static_cast<uint32_t>(static_cast<T>(~T(0)))
        : ((1u << bitsStored) - 1u);
    const uint32_t stored = static_cast<uint32_t>(raw) & mask;
    if (!isSigned) {
        return static_cast<int32_t>(stored);
    }
    const uint32_t signBit = 1u << (bitsStored - 1);
    return (stored & signBit)
        ? static_cast<int32_t>(stored) - static_cast<int32_t>(mask) - 1
        : static_cast<int32_t>(stored);
}

// --- 13-bit rescaled-HU code layout for lossy encoding ----------------------
//
// A lossy VarDCT encode of a full 16-bit-declared CT sample sees the real
// (windowed) signal as a tiny fraction of full scale: at a typical brain
// window the visible range is ~0.1% of 0..65535, so libjxl's perceptual
// distance model spends its whole budget nowhere near where the eye looks,
// and its distance floor caps achievable quality well above clinically
// tolerable error (measured: d=0.1 -> 12.8KB at brain-interior RMSE 9.2 HU,
// with quality unable to improve further as d drops).
//
// Declaring the true depth fixes this (see EncodeOptions::nominalBits), but
// that only helps if the sample values actually span close to that
// declared range. Raw signed HU (-1024..~3000 for most tissue, further for
// dense bone/metal) does not: 12-bit clips dense bone, and re-biasing HU by
// a power-of-two offset (the pre-existing scheme for non-rescaled data)
// leaves the codeword range mismatched to the real distribution.
//
// This layout instead re-quantises rescaled pixel data directly in HU units
// into a 13-bit codeword: code = round(HU) + kHu13Offset, valid codes
// 1..8191 (HU -3135..5055 inclusive), covering the full clinically relevant
// HU range for CT with headroom. Code 0 is reserved for pixel padding (PS3.3
// C.7.5.1.1.2 gives lossy compression an explicit padding value/range-limit
// mechanism precisely so a decoder can still identify padding after lossy
// requantisation - measured padding decodes to codes 0..38 at d=0.5, while
// real tissue never decodes below ~2100).
constexpr int32_t kHu13Offset = 3136;
constexpr int32_t kHu13Min = 1;
constexpr int32_t kHu13Max = 8191;
constexpr uint16_t kHu13PaddingCode = 0;
constexpr uint32_t kHu13NominalBits = 13;

// Parameters for one series/instance's rescaled-HU rewrite - everything
// ApplyHu13Rewrite needs to turn a raw stored sample into an HU13 code.
struct Hu13Params {
    double slope = 1.0;
    double intercept = 0.0;
    uint16_t bitsStored = 16;
    bool isSigned = false;

    // Inclusive padding band in STORED (pre-rescale) units, i.e. the same
    // units PixelPaddingValue/RangeLimit are expressed in. Both bounds equal
    // the single PixelPaddingValue when no PixelPaddingRangeLimit is present.
    bool hasPadding = false;
    int32_t paddingLo = 0;
    int32_t paddingHi = 0;
};

// Apply the HU13 rewrite in place across every sample in a buffer (all
// frames concatenated). Only meaningful for a lossy encode of a dataset that
// carries RescaleIntercept - see TranscodeToJxl/ApplyHu13DicomTags in
// transcode.cpp for the surrounding tag rewrite this pairs with.
template <typename T>
inline void ApplyHu13Rewrite(T* samples, size_t sampleCount, const Hu13Params& p) {
    for (size_t i = 0; i < sampleCount; ++i) {
        const int32_t stored = ExtractStoredValue<T>(samples[i], p.bitsStored, p.isSigned);
        uint16_t code;
        if (p.hasPadding && stored >= p.paddingLo && stored <= p.paddingHi) {
            code = kHu13PaddingCode;
        } else {
            const double hu = static_cast<double>(stored) * p.slope + p.intercept;
            long rounded = std::lround(hu) + kHu13Offset;
            rounded = std::max<long>(kHu13Min, std::min<long>(kHu13Max, rounded));
            code = static_cast<uint16_t>(rounded);
        }
        samples[i] = static_cast<T>(code);
    }
}

}  // namespace orthanc_jxl
