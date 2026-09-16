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

// Resolve the requested worker count into an actual libjxl worker-thread count.
// Returns 0 when no parallel runner should be used (single-threaded).
size_t ResolveThreadCount(int numWorkerThreads) {
    if (numWorkerThreads < 0) {
        return JxlThreadParallelRunnerDefaultNumWorkerThreads();
    }
    if (numWorkerThreads <= 1) {
        return 0;  // No runner; libjxl runs on the calling thread.
    }
    return static_cast<size_t>(numWorkerThreads);
}

// Return a libjxl parallel runner cached on the calling thread, (re)sized on
// demand. Reusing the runner avoids creating and destroying an OS thread pool
// on every encode/decode call - important when importing large studies.
// Returns nullptr when single-threaded operation was requested.
void* GetThreadLocalRunner(size_t numThreads) {
    if (numThreads == 0) {
        return nullptr;
    }
    thread_local JxlThreadParallelRunnerPtr runner;
    thread_local size_t cachedThreads = 0;
    if (!runner || cachedThreads != numThreads) {
        runner = JxlThreadParallelRunnerMake(nullptr, numThreads);
        cachedThreads = numThreads;
    }
    return runner.get();
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
    const EncodeOptions& options,
    int numWorkerThreads)
{
    // Create encoder with RAII wrapper
    auto encoder = JxlEncoderMake(nullptr);
    if (!encoder) {
        throw JxlCodecError("Failed to create JXL encoder");
    }

    // Reuse a thread-local parallel runner (avoids per-call pool churn)
    void* runner = GetThreadLocalRunner(ResolveThreadCount(numWorkerThreads));
    if (runner) {
        if (JxlEncoderSetParallelRunner(encoder.get(),
                                        JxlThreadParallelRunner,
                                        runner) != JXL_ENC_SUCCESS) {
            throw JxlCodecError("Failed to set parallel runner");
        }
    }

    // True only for an actual lossy encode (VarDCT with a non-zero distance).
    // A VarDCT config with distance == 0 is a lossless config (just a
    // different, faster-decoding lossless encoding than modular) and keeps
    // the settings below unchanged.
    const bool lossyVarDCT =
        (options.mode == EncodeMode::ProgressiveVarDCT && options.distance > 0.0f);

    // Nominal bit depth of the actual source data, vs. the UINT8/UINT16
    // container width PixelFormat implies. 0 (the default for every existing
    // lossless caller) means "same as the container" - no change from
    // historical behaviour. See EncodeOptions::nominalBits and the
    // JxlEncoderSetFrameBitDepth call below for why a narrower declared
    // width needs help getting there.
    const int containerBits = BitsPerSample(format);
    const uint32_t nominalBits =
        (options.nominalBits > 0 && options.nominalBits < static_cast<uint32_t>(containerBits))
            ? options.nominalBits : static_cast<uint32_t>(containerBits);

    // Set up basic info
    JxlBasicInfo basicInfo;
    JxlEncoderInitBasicInfo(&basicInfo);

    basicInfo.xsize = width;
    basicInfo.ysize = height;
    basicInfo.bits_per_sample = nominalBits;
    basicInfo.exponent_bits_per_sample = 0;  // Integer samples
    // uses_original_profile=TRUE tells libjxl to quantise in the ORIGINAL
    // (stored-unit) colour space - correct for every lossless mode, but for
    // lossy VarDCT it disables the perceptually-uniform XYB transform
    // entirely, so the transfer function below becomes irrelevant and the
    // encoder spends its distance budget in stored-unit space instead of a
    // perceptual one. Measured on one 512x512 signed CT: TRUE (the old
    // behaviour) gave d=1.0 -> 19.9KB at RMSE~147 (about a third of a
    // soft-tissue window); FALSE (XYB enabled) gave d=1.0 -> 10.2KB at
    // RMSE~34 for the offset data (RMSE~84 on the raw signed bits, before
    // the sign-offset fix below). Only flip this for an actual lossy encode -
    // lossless paths must stay bit-exact.
    basicInfo.uses_original_profile = lossyVarDCT ? JXL_FALSE : JXL_TRUE;
    basicInfo.num_color_channels = IsGrayscale(format) ? 1 : 3;
    basicInfo.num_extra_channels = 0;
    basicInfo.alpha_bits = 0;

    if (JxlEncoderSetBasicInfo(encoder.get(), &basicInfo) != JXL_ENC_SUCCESS) {
        throw JxlCodecError("Failed to set basic info");
    }

    // Describe the colour encoding. For lossless (modular + uses_original_profile)
    // this is metadata only - the integer samples are stored verbatim and no
    // transform is applied. It does matter for the lossy VarDCT path, so pick a
    // transfer function that matches the source rather than asserting one value
    // for everything:
    //   - grayscale medical data holds raw, linear-in-stored-units intensities
    //     (display is driven by the DICOM modality LUT / Rescale / Window, not
    //     an embedded profile), so signal LINEAR when uses_original_profile is
    //     TRUE (lossless / lossless-VarDCT);
    //   - for a lossy VarDCT encode uses_original_profile is FALSE (XYB is
    //     enabled, see above), which requires a real perceptual transfer
    //     function to be meaningful - use sRGB, matching the RGB case, per
    //     the measured numbers in the basicInfo comment above;
    //   - DICOM RGB without an ICC profile is assumed sRGB display data,
    //     regardless of mode (uses_original_profile is unaffected by
    //     RGB vs grayscale).
    // libjxl requires a known transfer function here (UNKNOWN is rejected when
    // uses_original_profile is set). Use RELATIVE (colorimetric) intent, which
    // suits measured data better than PERCEPTUAL.
    const bool gray = IsGrayscale(format);
    JxlColorEncoding colorEncoding = {};
    colorEncoding.color_space = gray ? JXL_COLOR_SPACE_GRAY : JXL_COLOR_SPACE_RGB;
    colorEncoding.white_point = JXL_WHITE_POINT_D65;
    colorEncoding.primaries = JXL_PRIMARIES_SRGB;  // ignored for grayscale
    colorEncoding.transfer_function =
        (gray && !lossyVarDCT) ? JXL_TRANSFER_FUNCTION_LINEAR : JXL_TRANSFER_FUNCTION_SRGB;
    colorEncoding.rendering_intent = JXL_RENDERING_INTENT_RELATIVE;

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
            // libjxl 0.12 (build 7a208214) defect, reproduced directly with
            // cjxl: PROGRESSIVE_DC >= 1 combined with PROGRESSIVE_AC fails the
            // encode outright whenever an explicit (non-auto, i.e. >= 0)
            // group-order centre is also set. This plugin's default
            // CenterFirstOrdering=true means a centre is set for essentially
            // every real image, so the combination is a config-time choice,
            // not a per-image one - clamp it here unconditionally rather than
            // let an encode fail, and warn once at plugin startup from the
            // parsed config (see PluginConfig::Parse in config.cpp), since
            // that's the only place that knows this is happening without
            // spamming a warning per image.
            if (options.progressiveAC && options.progressiveDC >= 1 &&
                (options.centerX >= 0 || options.centerY >= 0)) {
                // Defect combination: leave PROGRESSIVE_AC at the encoder's
                // own default (off) instead of propagating a hard failure.
            } else if (options.progressiveAC) {
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

    // libjxl's default input interpretation (JXL_BIT_DEPTH_FROM_PIXEL_FORMAT)
    // always rescales a UINT8/UINT16 buffer from the FULL container range
    // (255 / 65535), regardless of the declared bits_per_sample above -
    // measured: with that default, a 12-bit-nominal encode came back 16x too
    // small (values sat in 0..4095 while the encoder divided by 65535, i.e.
    // treated them as ~1/16 of full scale). JXL_BIT_DEPTH_FROM_CODESTREAM is
    // the encoder-side counterpart of the JxlDecoderSetImageOutBitDepth fix
    // in Decode() below: it tells the encoder the buffer already holds
    // unscaled codestream-range values (e.g. 0..8191 for a 13-bit nominal
    // depth), so no pixel-value rewriting is needed here at all - and,
    // unlike a manual pre-shift-by-power-of-two, it round-trips exactly
    // (a left-shift is not bit-identical to libjxl's own 65535/(2^n-1)
    // rescale, which broke exact lossless roundtrips at nominal depths that
    // aren't clean divisors of 16). A no-op (default kept) whenever
    // nominalBits == containerBits, which is every lossless caller.
    JxlBitDepth encodeBitDepth = {};
    encodeBitDepth.type = (nominalBits < static_cast<uint32_t>(containerBits))
        ? JXL_BIT_DEPTH_FROM_CODESTREAM : JXL_BIT_DEPTH_FROM_PIXEL_FORMAT;
    if (JxlEncoderSetFrameBitDepth(frameSettings, &encodeBitDepth) != JXL_ENC_SUCCESS) {
        throw JxlCodecError("Failed to set frame bit depth");
    }

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
    PixelFormat outputFormat,
    int numWorkerThreads)
{
    auto decoder = JxlDecoderMake(nullptr);
    if (!decoder) {
        throw JxlCodecError("Failed to create JXL decoder");
    }

    // Reuse a thread-local parallel runner (avoids per-call pool churn)
    void* runner = GetThreadLocalRunner(ResolveThreadCount(numWorkerThreads));
    if (runner) {
        JxlDecoderSetParallelRunner(decoder.get(), JxlThreadParallelRunner, runner);
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
                    // Without this, libjxl's default (JXL_BIT_DEPTH_FROM_PIXEL_FORMAT)
                    // scales output samples up to the full UINT8/UINT16 container
                    // range - a 13-bit codestream (bits_per_sample=13, see the
                    // encoder's nominalBits) would come back multiplied by
                    // 65535/8191 (~8x) instead of as the original codes. Requesting
                    // FROM_CODESTREAM here decodes into the codestream's native
                    // range instead, so a UINT16 buffer holds e.g. 0..8191
                    // unscaled. Must come after JxlDecoderSetImageOutBuffer - the
                    // API rejects it otherwise ("No image out buffer was set").
                    JxlBitDepth bitDepth = {};
                    bitDepth.type = JXL_BIT_DEPTH_FROM_CODESTREAM;
                    if (JxlDecoderSetImageOutBitDepth(decoder.get(), &bitDepth) != JXL_DEC_SUCCESS) {
                        throw JxlCodecError("Failed to set image output bit depth");
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
    PixelFormat outputFormat,
    int numWorkerThreads)
{
    return Decode(jxlData.data(), jxlData.size(), outputFormat, numWorkerThreads);
}

std::pair<std::vector<uint8_t>, ImageInfo> JxlCodec::Decode(
    const uint8_t* data, size_t size,
    int numWorkerThreads)
{
    ImageInfo info = DecodeInfo(data, size);
    std::vector<uint8_t> pixels = Decode(data, size, FormatFromImageInfo(info), numWorkerThreads);
    return {std::move(pixels), info};
}

std::pair<std::vector<uint8_t>, ImageInfo> JxlCodec::Decode(
    const std::vector<uint8_t>& jxlData,
    int numWorkerThreads)
{
    return Decode(jxlData.data(), jxlData.size(), numWorkerThreads);
}

} // namespace orthanc_jxl
