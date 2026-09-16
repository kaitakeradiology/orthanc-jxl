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

#include <orthanc/OrthancCPlugin.h>

#include "jxl_codec.h"
#include "dicom_handler.h"
#include "transfer_syntax.h"
#include "config.h"
#include "thread_pool.h"
#include "transcode.h"
#include "version.h"

// DCMTK decoders for compressed sources (JPEG, JPEG-LS). The plugin links its
// own dynamic libdcmdata whose codec registry is SEPARATE from Orthanc's
// (statically-linked) DCMTK, so we must register these ourselves for
// chooseRepresentation() to decode compressed inputs before JXL-encoding.
#include <dcmtk/dcmjpeg/djdecode.h>
#include <dcmtk/dcmjpls/djdecode.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace orthanc_jxl;

static OrthancPluginContext* context_ = nullptr;
static PluginConfig pluginConfig_;

// Shared worker pool for frame-level parallelism, reused for the plugin's
// lifetime so study imports don't pay per-instance thread-pool setup costs.
static std::unique_ptr<ThreadPool> threadPool_;

// ============================================================================
// Decode Image Callback
// ============================================================================

static OrthancPluginErrorCode DecodeImageCallback(
    OrthancPluginImage** target,
    const void* dicom,
    const uint32_t size,
    uint32_t frameIndex)
{
    try {
        DicomHandler handler(dicom, size);

        std::string transferSyntax = handler.GetTransferSyntax();
        if (!IsJxlTransferSyntax(transferSyntax)) {
            // Not our transfer syntax, let another decoder handle it
            return OrthancPluginErrorCode_NotImplemented;
        }

        // Get image info
        DicomImageInfo dicomInfo = handler.GetImageInfo();

        // Extract encapsulated JXL data
        std::vector<uint8_t> jxlData = handler.GetEncapsulatedData(frameIndex);

        // Decode JXL
        auto [pixels, jxlInfo] = JxlCodec::Decode(jxlData);
        PixelFormat format = JxlCodec::FormatFromImageInfo(jxlInfo);

        // Map to Orthanc pixel format
        OrthancPluginPixelFormat pixelFormat;
        switch (format) {
            case PixelFormat::Gray8:
                pixelFormat = OrthancPluginPixelFormat_Grayscale8;
                break;
            case PixelFormat::Gray16:
                pixelFormat = dicomInfo.isSigned
                    ? OrthancPluginPixelFormat_SignedGrayscale16
                    : OrthancPluginPixelFormat_Grayscale16;
                break;
            case PixelFormat::RGB24:
                pixelFormat = OrthancPluginPixelFormat_RGB24;
                break;
            default:  // RGB48
                pixelFormat = OrthancPluginPixelFormat_RGB48;
                break;
        }

        // Create Orthanc image
        OrthancPluginImage* image = OrthancPluginCreateImage(
            context_, pixelFormat, jxlInfo.width, jxlInfo.height);

        if (!image) {
            OrthancPluginLogError(context_, "orthanc-jxl: Failed to create output image");
            return OrthancPluginErrorCode_Plugin;
        }

        // Copy pixel data respecting pitch
        uint32_t pitch = OrthancPluginGetImagePitch(context_, image);
        uint8_t* buffer = reinterpret_cast<uint8_t*>(
            OrthancPluginGetImageBuffer(context_, image));

        if (!buffer || pitch == 0) {
            OrthancPluginFreeImage(context_, image);
            OrthancPluginLogError(context_, "orthanc-jxl: Failed to get image buffer");
            return OrthancPluginErrorCode_Plugin;
        }

        int bytesPerPixel = JxlCodec::BytesPerPixel(format);

        uint32_t rowSize = jxlInfo.width * bytesPerPixel;

        for (uint32_t y = 0; y < jxlInfo.height; ++y) {
            memcpy(buffer + y * pitch,
                   pixels.data() + y * rowSize,
                   rowSize);
        }

        *target = image;
        return OrthancPluginErrorCode_Success;

    } catch (const std::exception& e) {
        OrthancPluginLogError(context_, (std::string("orthanc-jxl decode error: ") + e.what()).c_str());
        return OrthancPluginErrorCode_Plugin;
    }
}

// ============================================================================
// Transcoder Callback
// ============================================================================

static OrthancPluginErrorCode TranscoderCallback(
    OrthancPluginMemoryBuffer* transcoded,
    const void* buffer,
    uint64_t size,
    const char* const* allowedSyntaxes,
    uint32_t countSyntaxes,
    uint8_t allowNewSopInstanceUid)
{
    // Check what transfer syntaxes are requested. wants110/wants112 are kept
    // separate (rather than a single jxlRequested flag) because which one is
    // offered decides whether we may produce genuinely lossy bits at all -
    // see the request->encode decision below.
    bool wants110 = false;  // JPEG XL Lossless
    bool wants112 = false;  // JPEG XL (this plugin's convention: lossy output)
    const char* uncompressedSyntax = nullptr;

    for (uint32_t i = 0; i < countSyntaxes; ++i) {
        if (strcmp(allowedSyntaxes[i], TS_JPEG_XL_LOSSLESS) == 0) {
            wants110 = true;
        }
        if (strcmp(allowedSyntaxes[i], TS_JPEG_XL) == 0) {
            wants112 = true;
        }
        // Track first uncompressed syntax for FROM-JXL transcoding
        if (!uncompressedSyntax && IsUncompressedTransferSyntax(allowedSyntaxes[i])) {
            uncompressedSyntax = allowedSyntaxes[i];
        }
    }

    const bool configLossy =
        (pluginConfig_.encodeOptions.mode == EncodeMode::ProgressiveVarDCT &&
         pluginConfig_.encodeOptions.distance > 0.0f);

    try {
        DicomHandler handler(buffer, static_cast<size_t>(size));
        std::string currentTs = handler.GetTransferSyntax();

        // Case 1: Source is JXL and uncompressed output is requested (FROM-JXL)
        if (IsJxlTransferSyntax(currentTs) && uncompressedSyntax) {
            TranscodeResult result = TranscodeFromJxl(
                buffer, static_cast<size_t>(size), uncompressedSyntax, *threadPool_);

            if (OrthancPluginCreateMemoryBuffer(context_, transcoded, result.dicom.size())
                != OrthancPluginErrorCode_Success) {
                OrthancPluginLogError(context_, "orthanc-jxl: Failed to allocate output buffer");
                return OrthancPluginErrorCode_Plugin;
            }
            memcpy(transcoded->data, result.dicom.data(), result.dicom.size());

            char logMsg[256];
            snprintf(logMsg, sizeof(logMsg),
                "orthanc-jxl: Transcoded FROM JXL (%u frame%s) -> %zu KB",
                result.frameCount, result.frameCount == 1 ? "" : "s",
                result.nativeBytes / 1024);
            OrthancPluginLogInfo(context_, logMsg);

            return OrthancPluginErrorCode_Success;
        }

        // Case 2: JXL is requested and source is not JXL (TO-JXL)
        if ((wants110 || wants112) && !IsJxlTransferSyntax(currentTs)) {
            // Decide what to actually produce. A request naming .112 while
            // configured for lossy VarDCT produces genuinely lossy bits under
            // .112. A request only naming .110 must never receive lossy bits
            // under the Lossless UID, so force a lossless encode even when
            // the plugin is configured for lossy output. A request only
            // naming .112 while NOT configured for lossy output is declined:
            // this plugin's convention is .110=lossless, .112=lossy, and
            // labelling lossless bits as .112 would be the same mislabel in
            // the other direction.
            PluginConfig effectiveConfig = pluginConfig_;
            bool willEncodeLossy = false;

            if (wants112 && configLossy) {
                willEncodeLossy = true;
            } else if (wants110) {
                if (configLossy) {
                    effectiveConfig.encodeOptions.mode = EncodeMode::ProgressiveLossless;
                    effectiveConfig.encodeOptions.distance = 0.0f;
                    // The SDK exposes only Error/Warning/Info levels; Info is
                    // the closest available to "debug" for a routine,
                    // expected fallback (a requester offering only the
                    // Lossless UID is normal, not misconfiguration).
                    OrthancPluginLogInfo(context_,
                        "orthanc-jxl: configured for lossy VarDCT but only "
                        "1.2.840.10008.1.2.4.110 (Lossless) was requested - "
                        "encoding ProgressiveLossless instead of mislabeling "
                        "lossy bits as Lossless");
                }
                willEncodeLossy = false;
            } else {
                // Only .112 offered, config is not lossy: decline rather than
                // mislabel lossless bits as .112.
                return OrthancPluginErrorCode_NotImplemented;
            }

            if (willEncodeLossy && !allowNewSopInstanceUid) {
                // A lossy encode always assigns a new SOPInstanceUID (PS3.3
                // C.7.6.1.1.5-adjacent identity change - see
                // ApplyCommonLossyTags/GenerateNewSopInstanceUid). Orthanc's
                // contract does not allow that when allowNewSopInstanceUid is
                // false, so decline rather than silently break identity.
                OrthancPluginLogWarning(context_,
                    "orthanc-jxl: declining lossy TO-JXL - Orthanc did not "
                    "allow a new SOPInstanceUID for this transcode");
                return OrthancPluginErrorCode_NotImplemented;
            }

            TranscodeResult result;
            try {
                result = TranscodeToJxl(
                    buffer, static_cast<size_t>(size), effectiveConfig, *threadPool_,
                    pluginConfig_.SingleFrameThreads());
            } catch (const std::exception& e) {
                // Can't decode/encode this source (e.g. a compressed syntax with
                // no registered decoder, such as JPEG2000). Decline so Orthanc
                // keeps the original instance (ingest) or reports it cannot
                // transcode (/modify), instead of failing the whole operation —
                // important once IngestTranscodingOfCompressed is enabled.
                OrthancPluginLogWarning(context_,
                    (std::string("orthanc-jxl: declining TO-JXL (") + e.what() + ")").c_str());
                return OrthancPluginErrorCode_NotImplemented;
            }

            if (OrthancPluginCreateMemoryBuffer(context_, transcoded, result.dicom.size())
                != OrthancPluginErrorCode_Success) {
                OrthancPluginLogError(context_, "orthanc-jxl: Failed to allocate output buffer");
                return OrthancPluginErrorCode_Plugin;
            }
            memcpy(transcoded->data, result.dicom.data(), result.dicom.size());

            double ratio = result.encodedBytes
                ? static_cast<double>(result.nativeBytes) / result.encodedBytes : 0.0;
            char logMsg[256];
            snprintf(logMsg, sizeof(logMsg),
                "orthanc-jxl: Transcoded TO JXL (%u frame%s) %zu KB -> %zu KB (%.2fx)",
                result.frameCount, result.frameCount == 1 ? "" : "s",
                result.nativeBytes / 1024, result.encodedBytes / 1024, ratio);
            OrthancPluginLogInfo(context_, logMsg);

            return OrthancPluginErrorCode_Success;
        }

        // Case 3: Identity - a stored JXL instance is requested in its own
        // transfer syntax (serve / forward / stream). Orthanc still routes this
        // through the transcoder; pass the source through unchanged rather than
        // declining, because declining makes Orthanc core throw "Unsupported
        // transfer syntax".
        if (IsJxlTransferSyntax(currentTs)) {
            for (uint32_t i = 0; i < countSyntaxes; ++i) {
                if (currentTs == allowedSyntaxes[i]) {
                    if (OrthancPluginCreateMemoryBuffer(context_, transcoded,
                            static_cast<uint32_t>(size)) != OrthancPluginErrorCode_Success) {
                        OrthancPluginLogError(context_, "orthanc-jxl: identity buffer alloc failed");
                        return OrthancPluginErrorCode_Plugin;
                    }
                    memcpy(transcoded->data, buffer, static_cast<size_t>(size));
                    return OrthancPluginErrorCode_Success;
                }
            }
        }

        // Not our job
        return OrthancPluginErrorCode_NotImplemented;

    } catch (const std::exception& e) {
        OrthancPluginLogError(context_,
            (std::string("orthanc-jxl transcode error: ") + e.what()).c_str());
        return OrthancPluginErrorCode_Plugin;
    }
}

// ============================================================================
// Plugin Entry Points
// ============================================================================

extern "C" {

ORTHANC_PLUGINS_API int32_t OrthancPluginInitialize(OrthancPluginContext* context)
{
    context_ = context;

    // Check Orthanc version
    if (OrthancPluginCheckVersion(context) == 0) {
        std::string msg = "orthanc-jxl: This plugin requires Orthanc >= " +
            std::to_string(ORTHANC_PLUGINS_MINIMAL_MAJOR_NUMBER) + "." +
            std::to_string(ORTHANC_PLUGINS_MINIMAL_MINOR_NUMBER) + "." +
            std::to_string(ORTHANC_PLUGINS_MINIMAL_REVISION_NUMBER);
        OrthancPluginLogError(context, msg.c_str());
        return -1;
    }

    OrthancPluginSetDescription2(context, PLUGIN_NAME, PLUGIN_DESCRIPTION);

    // Create the shared worker pool for frame-level parallelism.
    unsigned int hw = std::thread::hardware_concurrency();
    threadPool_ = std::make_unique<ThreadPool>(hw == 0 ? 1u : hw);

    // Parse plugin configuration
    char* configJson = OrthancPluginGetConfiguration(context);
    if (configJson) {
        pluginConfig_ = PluginConfig::Parse(configJson);
        OrthancPluginFreeString(context, configJson);
    } else {
        pluginConfig_ = PluginConfig::Default();
    }

    // Log configuration
    const char* modeName = "Unknown";
    switch (pluginConfig_.encodeOptions.mode) {
        case EncodeMode::Lossless: modeName = "Lossless"; break;
        case EncodeMode::ProgressiveLossless: modeName = "ProgressiveLossless"; break;
        case EncodeMode::ProgressiveVarDCT: modeName = "ProgressiveVarDCT"; break;
    }
    // The TS a TO-JXL encode actually lands on for THIS config (matches the
    // `lossy` decision in transcode.cpp): only a VarDCT config with a
    // non-zero distance ever produces .112, everything else is .110.
    const bool startupLossy =
        (pluginConfig_.encodeOptions.mode == EncodeMode::ProgressiveVarDCT &&
         pluginConfig_.encodeOptions.distance > 0.0f);
    const char* targetTs = startupLossy ? TS_JPEG_XL : TS_JPEG_XL_LOSSLESS;
    char configMsg[320];
    snprintf(configMsg, sizeof(configMsg),
        "orthanc-jxl: Config - Mode=%s, Effort=%d, Distance=%.2f, EncodeThreads=%d, "
        "Pool=%u, TargetTS=%s",
        modeName, pluginConfig_.encodeOptions.effort, pluginConfig_.encodeOptions.distance,
        pluginConfig_.encodeThreads, threadPool_ ? (unsigned)threadPool_->Size() : 0u,
        targetTs);
    OrthancPluginLogInfo(context, configMsg);

    if (pluginConfig_.progressiveAcDefectWillBeClamped) {
        // See jxl_codec.cpp (ProgressiveVarDCT case) and config.cpp
        // (progressiveAcDefectWillBeClamped) for the full story: libjxl 0.12
        // fails the encode outright for this combination, so the codec
        // always clamps PROGRESSIVE_AC off regardless of this warning - it
        // exists purely so an admin who set ProgressiveAC=true knows why it
        // isn't taking effect.
        OrthancPluginLogWarning(context,
            "orthanc-jxl: ProgressiveAC=true with ProgressiveDC>=1 and "
            "CenterFirstOrdering=true hits a libjxl 0.12 defect (the encode "
            "fails outright) - PROGRESSIVE_AC will be silently disabled for "
            "every VarDCT encode");
    }

    // Register DCMTK decoders so the transcoder can decode compressed sources
    // (JPEG / JPEG-LS) to native before JXL-encoding. Idempotent.
    DJDecoderRegistration::registerCodecs();
    DJLSDecoderRegistration::registerCodecs();

    // Register decode callback for viewing JXL images
    OrthancPluginRegisterDecodeImageCallback(context, DecodeImageCallback);

    // Register transcoder callback for encoding to JXL
    OrthancPluginRegisterTranscoderCallback(context, TranscoderCallback);

    OrthancPluginLogInfo(context,
        "orthanc-jxl: Plugin initialized - JPEG-XL transfer syntaxes enabled");
    OrthancPluginLogInfo(context,
        "orthanc-jxl: Supported: 1.2.840.10008.1.2.4.110 (Lossless), "
        "1.2.840.10008.1.2.4.111 (JPEG Recompression), "
        "1.2.840.10008.1.2.4.112 (Lossy)");

    return 0;
}

ORTHANC_PLUGINS_API void OrthancPluginFinalize()
{
    DJLSDecoderRegistration::cleanup();
    DJDecoderRegistration::cleanup();
    threadPool_.reset();
    OrthancPluginLogInfo(context_, "orthanc-jxl: Plugin finalized");
    context_ = nullptr;
}

ORTHANC_PLUGINS_API const char* OrthancPluginGetName()
{
    return PLUGIN_NAME;
}

ORTHANC_PLUGINS_API const char* OrthancPluginGetVersion()
{
    return PLUGIN_VERSION;
}

} // extern "C"
