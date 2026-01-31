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

#include <cstdint>
#include <vector>
#include <stdexcept>
#include <string>
#include <utility>

namespace orthanc_jxl {

class JxlCodecError : public std::runtime_error {
public:
    explicit JxlCodecError(const std::string& msg) : std::runtime_error(msg) {}
};

enum class PixelFormat {
    Gray8,      // 8-bit grayscale
    Gray16,     // 16-bit grayscale (common in medical imaging)
    RGB24,      // 8-bit RGB (24bpp)
    RGB48       // 16-bit RGB (48bpp)
};

enum class EncodeMode {
    Lossless,           // Pure lossless modular mode
    ProgressiveLossless, // Lossless with squeeze transform
    ProgressiveVarDCT   // VarDCT mode (near-lossless)
};

struct EncodeOptions {
    EncodeMode mode = EncodeMode::ProgressiveLossless;
    int effort = 7;          // 1-10, default 7 balances speed and compression
    int centerX = -1;        // Center for group ordering (-1 = auto)
    int centerY = -1;
    int progressiveDC = 0;   // VarDCT only (0-2)
    bool progressiveAC = false;
    float distance = 0.0f;   // 0.0 = mathematically lossless

    static EncodeOptions Lossless(int effort = 7);
    static EncodeOptions ProgressiveLossless(int effort = 7, int centerX = -1, int centerY = -1);
    static EncodeOptions ProgressiveVarDCT(int effort = 7, float distance = 0.0f,
                                           int centerX = -1, int centerY = -1,
                                           int progressiveDC = 1, bool progressiveAC = true);
};

struct ImageInfo {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t bitsPerSample = 0;
    uint32_t numChannels = 0;
    bool isGrayscale = false;
};

class JxlCodec {
public:
    // Encoding
    static std::vector<uint8_t> Encode(
        const void* pixelData,
        uint32_t width,
        uint32_t height,
        PixelFormat format,
        const EncodeOptions& options = EncodeOptions::ProgressiveLossless()
    );

    // Convenience encoders
    static std::vector<uint8_t> EncodeLossless(
        const void* pixelData, uint32_t width, uint32_t height,
        PixelFormat format, int effort = 7
    );

    static std::vector<uint8_t> EncodeProgressiveLossless(
        const void* pixelData, uint32_t width, uint32_t height,
        PixelFormat format, int effort = 7, int centerX = -1, int centerY = -1
    );

    // Decoding - info only (fast)
    static ImageInfo DecodeInfo(const uint8_t* data, size_t size);
    static ImageInfo DecodeInfo(const std::vector<uint8_t>& jxlData);

    // Decoding - full decode with specified format
    static std::vector<uint8_t> Decode(
        const uint8_t* data, size_t size,
        PixelFormat outputFormat
    );
    static std::vector<uint8_t> Decode(
        const std::vector<uint8_t>& jxlData,
        PixelFormat outputFormat
    );

    // Decoding - auto-detect format
    static std::pair<std::vector<uint8_t>, ImageInfo> Decode(
        const uint8_t* data, size_t size
    );
    static std::pair<std::vector<uint8_t>, ImageInfo> Decode(
        const std::vector<uint8_t>& jxlData
    );

    // Helpers
    static int BytesPerPixel(PixelFormat format);
    static int NumChannels(PixelFormat format);
    static int BitsPerSample(PixelFormat format);
    static bool IsGrayscale(PixelFormat format);
    static PixelFormat FormatFromImageInfo(const ImageInfo& info);
};

} // namespace orthanc_jxl
