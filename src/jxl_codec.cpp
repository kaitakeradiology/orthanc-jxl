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

#include "jxl_codec.h"

#include <jxl/encode.h>
#include <jxl/encode_cxx.h>
#include <jxl/decode.h>
#include <jxl/decode_cxx.h>
#include <jxl/thread_parallel_runner.h>
#include <jxl/thread_parallel_runner_cxx.h>

namespace orthanc_jxl {

// ============================================================================
// EncodeOptions factory methods
// ============================================================================

EncodeOptions EncodeOptions::Lossless(int effort) {
    return {EncodeMode::Lossless, effort};
}

EncodeOptions EncodeOptions::ProgressiveLossless(int effort, int centerX, int centerY) {
    return {EncodeMode::ProgressiveLossless, effort, centerX, centerY};
}

EncodeOptions EncodeOptions::ProgressiveVarDCT(int effort, float distance,
                                               int centerX, int centerY,
                                               int progressiveDC, bool progressiveAC) {
    return {EncodeMode::ProgressiveVarDCT, effort, centerX, centerY, progressiveDC, progressiveAC, distance};
}

// ============================================================================
// Helper functions
// ============================================================================

namespace {
struct PixelFormatInfo {
    int bytesPerPixel;
    int numChannels;
    int bitsPerSample;
    bool grayscale;
    JxlDataType jxlType;
};

constexpr PixelFormatInfo kFormatInfo[] = {
    {1, 1, 8,  true,  JXL_TYPE_UINT8},   // Gray8
    {2, 1, 16, true,  JXL_TYPE_UINT16},  // Gray16
    {3, 3, 8,  false, JXL_TYPE_UINT8},   // RGB24
    {6, 3, 16, false, JXL_TYPE_UINT16},  // RGB48
};

inline const PixelFormatInfo& GetFormatInfo(PixelFormat format) {
    return kFormatInfo[static_cast<int>(format)];
}
} // anonymous namespace

int JxlCodec::BytesPerPixel(PixelFormat format) { return GetFormatInfo(format).bytesPerPixel; }
int JxlCodec::NumChannels(PixelFormat format) { return GetFormatInfo(format).numChannels; }
int JxlCodec::BitsPerSample(PixelFormat format) { return GetFormatInfo(format).bitsPerSample; }
bool JxlCodec::IsGrayscale(PixelFormat format) { return GetFormatInfo(format).grayscale; }

PixelFormat JxlCodec::FormatFromImageInfo(const ImageInfo& info) {
    if (info.isGrayscale) {
        return (info.bitsPerSample <= 8) ? PixelFormat::Gray8 : PixelFormat::Gray16;
    }
    return (info.bitsPerSample <= 8) ? PixelFormat::RGB24 : PixelFormat::RGB48;
}

static JxlDataType ToJxlDataType(PixelFormat format) {
    return GetFormatInfo(format).jxlType;
}

// ============================================================================
// Encoding
// ============================================================================

std::vector<uint8_t> JxlCodec::Encode(
    const void* pixelData,
    uint32_t width,
    uint32_t height,
    PixelFormat format,
    const EncodeOptions& options)
{
    // Create encoder with RAII wrapper
    auto encoder = JxlEncoderMake(nullptr);
    if (!encoder) {
        throw JxlCodecError("Failed to create JXL encoder");
    }

    // Set up parallel runner for multi-threaded encoding
    auto runner = JxlThreadParallelRunnerMake(
        nullptr,
        JxlThreadParallelRunnerDefaultNumWorkerThreads()
    );
    if (runner) {
        if (JxlEncoderSetParallelRunner(encoder.get(),
                                        JxlThreadParallelRunner,
                                        runner.get()) != JXL_ENC_SUCCESS) {
            throw JxlCodecError("Failed to set parallel runner");
        }
    }

    // Set up basic info
    JxlBasicInfo basicInfo;
    JxlEncoderInitBasicInfo(&basicInfo);

    basicInfo.xsize = width;
    basicInfo.ysize = height;
    basicInfo.bits_per_sample = BitsPerSample(format);
    basicInfo.exponent_bits_per_sample = 0;  // Integer samples
    basicInfo.uses_original_profile = JXL_TRUE;  // Preserve values for medical imaging
    basicInfo.num_color_channels = IsGrayscale(format) ? 1 : 3;
    basicInfo.num_extra_channels = 0;
    basicInfo.alpha_bits = 0;

    if (JxlEncoderSetBasicInfo(encoder.get(), &basicInfo) != JXL_ENC_SUCCESS) {
        throw JxlCodecError("Failed to set basic info");
    }

    // Set color encoding
    JxlColorEncoding colorEncoding = {};
    colorEncoding.color_space = IsGrayscale(format) ? JXL_COLOR_SPACE_GRAY : JXL_COLOR_SPACE_RGB;
    colorEncoding.white_point = JXL_WHITE_POINT_D65;
    colorEncoding.primaries = JXL_PRIMARIES_SRGB;
    colorEncoding.transfer_function = JXL_TRANSFER_FUNCTION_LINEAR;  // Linear for medical imaging
    colorEncoding.rendering_intent = JXL_RENDERING_INTENT_PERCEPTUAL;

    if (JxlEncoderSetColorEncoding(encoder.get(), &colorEncoding) != JXL_ENC_SUCCESS) {
        throw JxlCodecError("Failed to set color encoding");
    }

    // Create frame settings
    JxlEncoderFrameSettings* frameSettings = JxlEncoderFrameSettingsCreate(encoder.get(), nullptr);
    if (!frameSettings) {
        throw JxlCodecError("Failed to create frame settings");
    }

    // Configure based on encoding mode
    switch (options.mode) {
        case EncodeMode::Lossless:
            JxlEncoderSetFrameLossless(frameSettings, JXL_TRUE);
            JxlEncoderSetFrameDistance(frameSettings, 0.0f);
            JxlEncoderFrameSettingsSetOption(frameSettings, JXL_ENC_FRAME_SETTING_MODULAR, 1);
            JxlEncoderFrameSettingsSetOption(frameSettings, JXL_ENC_FRAME_SETTING_RESPONSIVE, 0);
            break;

        case EncodeMode::ProgressiveLossless:
            JxlEncoderSetFrameLossless(frameSettings, JXL_TRUE);
            JxlEncoderSetFrameDistance(frameSettings, 0.0f);
            JxlEncoderFrameSettingsSetOption(frameSettings, JXL_ENC_FRAME_SETTING_MODULAR, 1);
            JxlEncoderFrameSettingsSetOption(frameSettings, JXL_ENC_FRAME_SETTING_RESPONSIVE, 1);
            // Center-first group ordering for streaming
            JxlEncoderFrameSettingsSetOption(frameSettings, JXL_ENC_FRAME_SETTING_GROUP_ORDER, 1);
            JxlEncoderFrameSettingsSetOption(frameSettings, JXL_ENC_FRAME_SETTING_GROUP_ORDER_CENTER_X,
                                             options.centerX);
            JxlEncoderFrameSettingsSetOption(frameSettings, JXL_ENC_FRAME_SETTING_GROUP_ORDER_CENTER_Y,
                                             options.centerY);
            break;

        case EncodeMode::ProgressiveVarDCT:
            if (options.distance == 0.0f) {
                JxlEncoderSetFrameLossless(frameSettings, JXL_TRUE);
            } else {
                JxlEncoderSetFrameLossless(frameSettings, JXL_FALSE);
            }
            JxlEncoderSetFrameDistance(frameSettings, options.distance);
            JxlEncoderFrameSettingsSetOption(frameSettings, JXL_ENC_FRAME_SETTING_MODULAR, 0);
            JxlEncoderFrameSettingsSetOption(frameSettings, JXL_ENC_FRAME_SETTING_PROGRESSIVE_DC,
                                             options.progressiveDC);
            if (options.progressiveAC) {
                JxlEncoderFrameSettingsSetOption(frameSettings, JXL_ENC_FRAME_SETTING_PROGRESSIVE_AC, 1);
            }
            JxlEncoderFrameSettingsSetOption(frameSettings, JXL_ENC_FRAME_SETTING_GROUP_ORDER, 1);
            JxlEncoderFrameSettingsSetOption(frameSettings, JXL_ENC_FRAME_SETTING_GROUP_ORDER_CENTER_X,
                                             options.centerX);
            JxlEncoderFrameSettingsSetOption(frameSettings, JXL_ENC_FRAME_SETTING_GROUP_ORDER_CENTER_Y,
                                             options.centerY);
            break;

        default:
            // Default to lossless for safety
            JxlEncoderSetFrameLossless(frameSettings, JXL_TRUE);
            JxlEncoderSetFrameDistance(frameSettings, 0.0f);
            break;
    }

    // Set effort level
    JxlEncoderFrameSettingsSetOption(frameSettings, JXL_ENC_FRAME_SETTING_EFFORT, options.effort);

    // Set up pixel format
    JxlPixelFormat pixelFormat = {};
    pixelFormat.num_channels = NumChannels(format);
    pixelFormat.data_type = ToJxlDataType(format);
    pixelFormat.endianness = JXL_NATIVE_ENDIAN;
    pixelFormat.align = 0;

    // Calculate input buffer size
    size_t inputSize = static_cast<size_t>(width) * height * BytesPerPixel(format);

    // Add the image frame
    if (JxlEncoderAddImageFrame(frameSettings, &pixelFormat, pixelData, inputSize) != JXL_ENC_SUCCESS) {
        throw JxlCodecError("Failed to add image frame");
    }

    // Close input
    JxlEncoderCloseInput(encoder.get());

    // Process output
    std::vector<uint8_t> result(64 * 1024);  // Start with 64KB
    uint8_t* nextOut = result.data();
    size_t availOut = result.size();

    while (true) {
        JxlEncoderStatus status = JxlEncoderProcessOutput(encoder.get(), &nextOut, &availOut);

        if (status == JXL_ENC_SUCCESS) {
            size_t actualSize = result.size() - availOut;
            result.resize(actualSize);
            break;
        } else if (status == JXL_ENC_NEED_MORE_OUTPUT) {
            size_t bytesWritten = result.size() - availOut;
            result.resize(result.size() * 2);
            nextOut = result.data() + bytesWritten;
            availOut = result.size() - bytesWritten;
        } else {
            throw JxlCodecError("Encoding failed with error: " + std::to_string(static_cast<int>(status)));
        }
    }

    return result;
}

std::vector<uint8_t> JxlCodec::EncodeLossless(
    const void* pixelData, uint32_t width, uint32_t height,
    PixelFormat format, int effort)
{
    return Encode(pixelData, width, height, format, EncodeOptions::Lossless(effort));
}

std::vector<uint8_t> JxlCodec::EncodeProgressiveLossless(
    const void* pixelData, uint32_t width, uint32_t height,
    PixelFormat format, int effort, int centerX, int centerY)
{
    return Encode(pixelData, width, height, format,
                  EncodeOptions::ProgressiveLossless(effort, centerX, centerY));
}

// ============================================================================
// Decoding - Info only
// ============================================================================

ImageInfo JxlCodec::DecodeInfo(const uint8_t* data, size_t size) {
    auto decoder = JxlDecoderMake(nullptr);
    if (!decoder) {
        throw JxlCodecError("Failed to create JXL decoder");
    }

    if (JxlDecoderSubscribeEvents(decoder.get(), JXL_DEC_BASIC_INFO) != JXL_DEC_SUCCESS) {
        throw JxlCodecError("Failed to subscribe to decoder events");
    }

    if (JxlDecoderSetInput(decoder.get(), data, size) != JXL_DEC_SUCCESS) {
        throw JxlCodecError("Failed to set decoder input");
    }

    while (true) {
        JxlDecoderStatus status = JxlDecoderProcessInput(decoder.get());

        if (status == JXL_DEC_BASIC_INFO) {
            JxlBasicInfo basicInfo;
            if (JxlDecoderGetBasicInfo(decoder.get(), &basicInfo) != JXL_DEC_SUCCESS) {
                throw JxlCodecError("Failed to get basic info");
            }

            ImageInfo info;
            info.width = basicInfo.xsize;
            info.height = basicInfo.ysize;
            info.bitsPerSample = basicInfo.bits_per_sample;
            info.numChannels = basicInfo.num_color_channels;
            info.isGrayscale = (basicInfo.num_color_channels == 1);
            return info;
        } else if (status == JXL_DEC_ERROR) {
            throw JxlCodecError("Decoder error while reading info");
        } else if (status == JXL_DEC_NEED_MORE_INPUT) {
            throw JxlCodecError("Incomplete JXL data");
        }
    }
}

ImageInfo JxlCodec::DecodeInfo(const std::vector<uint8_t>& jxlData) {
    return DecodeInfo(jxlData.data(), jxlData.size());
}

// ============================================================================
// Decoding - Full decode
// ============================================================================

std::vector<uint8_t> JxlCodec::Decode(
    const uint8_t* data, size_t size,
    PixelFormat outputFormat)
{
    auto decoder = JxlDecoderMake(nullptr);
    if (!decoder) {
        throw JxlCodecError("Failed to create JXL decoder");
    }

    // Set up parallel runner
    auto runner = JxlThreadParallelRunnerMake(
        nullptr,
        JxlThreadParallelRunnerDefaultNumWorkerThreads()
    );
    if (runner) {
        JxlDecoderSetParallelRunner(decoder.get(), JxlThreadParallelRunner, runner.get());
    }

    if (JxlDecoderSubscribeEvents(decoder.get(),
            JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE) != JXL_DEC_SUCCESS) {
        throw JxlCodecError("Failed to subscribe to decoder events");
    }

    if (JxlDecoderSetInput(decoder.get(), data, size) != JXL_DEC_SUCCESS) {
        throw JxlCodecError("Failed to set decoder input");
    }

    std::vector<uint8_t> result;
    uint32_t width = 0, height = 0;
    bool outputBufferSet = false;

    JxlPixelFormat pixelFormat = {};
    pixelFormat.num_channels = NumChannels(outputFormat);
    pixelFormat.data_type = ToJxlDataType(outputFormat);
    pixelFormat.endianness = JXL_NATIVE_ENDIAN;
    pixelFormat.align = 0;

    while (true) {
        JxlDecoderStatus status = JxlDecoderProcessInput(decoder.get());

        switch (status) {
            case JXL_DEC_BASIC_INFO: {
                JxlBasicInfo basicInfo;
                if (JxlDecoderGetBasicInfo(decoder.get(), &basicInfo) != JXL_DEC_SUCCESS) {
                    throw JxlCodecError("Failed to get basic info");
                }
                width = basicInfo.xsize;
                height = basicInfo.ysize;
                size_t bufferSize = static_cast<size_t>(width) * height * BytesPerPixel(outputFormat);
                result.resize(bufferSize);
                break;
            }

            case JXL_DEC_NEED_IMAGE_OUT_BUFFER: {
                if (!outputBufferSet) {
                    size_t requiredSize;
                    if (JxlDecoderImageOutBufferSize(decoder.get(), &pixelFormat, &requiredSize) != JXL_DEC_SUCCESS) {
                        throw JxlCodecError("Failed to get output buffer size");
                    }
                    if (result.size() < requiredSize) {
                        result.resize(requiredSize);
                    }
                    if (JxlDecoderSetImageOutBuffer(decoder.get(), &pixelFormat,
                                                    result.data(), result.size()) != JXL_DEC_SUCCESS) {
                        throw JxlCodecError("Failed to set output buffer");
                    }
                    outputBufferSet = true;
                }
                break;
            }

            case JXL_DEC_FULL_IMAGE:
                return result;

            case JXL_DEC_SUCCESS:
                return result;

            case JXL_DEC_ERROR:
                throw JxlCodecError("Decoder error");

            case JXL_DEC_NEED_MORE_INPUT:
                throw JxlCodecError("Incomplete JXL data");

            default:
                // Continue processing other events
                break;
        }
    }
}

std::vector<uint8_t> JxlCodec::Decode(
    const std::vector<uint8_t>& jxlData,
    PixelFormat outputFormat)
{
    return Decode(jxlData.data(), jxlData.size(), outputFormat);
}

std::pair<std::vector<uint8_t>, ImageInfo> JxlCodec::Decode(
    const uint8_t* data, size_t size)
{
    ImageInfo info = DecodeInfo(data, size);
    std::vector<uint8_t> pixels = Decode(data, size, FormatFromImageInfo(info));
    return {std::move(pixels), info};
}

std::pair<std::vector<uint8_t>, ImageInfo> JxlCodec::Decode(
    const std::vector<uint8_t>& jxlData)
{
    return Decode(jxlData.data(), jxlData.size());
}

} // namespace orthanc_jxl
