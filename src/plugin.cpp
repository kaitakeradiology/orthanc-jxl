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
#include "version.h"

#include <cstdio>
#include <cstring>
#include <string>

using namespace orthanc_jxl;

static OrthancPluginContext* context_ = nullptr;

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
    (void)allowNewSopInstanceUid;  // Not used for JXL transcoding

    // Check what transfer syntaxes are requested
    bool jxlRequested = false;
    const char* uncompressedSyntax = nullptr;

    for (uint32_t i = 0; i < countSyntaxes; ++i) {
        if (strcmp(allowedSyntaxes[i], TS_JPEG_XL_LOSSLESS) == 0) {
            jxlRequested = true;
        }
        // Track first uncompressed syntax for FROM-JXL transcoding
        if (!uncompressedSyntax && IsUncompressedTransferSyntax(allowedSyntaxes[i])) {
            uncompressedSyntax = allowedSyntaxes[i];
        }
    }

    try {
        DicomHandler handler(buffer, static_cast<size_t>(size));
        std::string currentTs = handler.GetTransferSyntax();

        // Case 1: Source is JXL and uncompressed output is requested (FROM-JXL)
        if (IsJxlTransferSyntax(currentTs) && uncompressedSyntax) {
            // Extract encapsulated JXL data
            std::vector<uint8_t> jxlData = handler.GetEncapsulatedData(0);

            // Decode JXL
            auto [pixels, jxlInfo] = JxlCodec::Decode(jxlData);

            // Set native pixel data
            handler.SetNativePixelData(pixels);
            handler.SetTransferSyntax(uncompressedSyntax);

            // Write to buffer
            std::vector<uint8_t> output = handler.WriteToBuffer(uncompressedSyntax);

            // Allocate Orthanc buffer
            if (OrthancPluginCreateMemoryBuffer(context_, transcoded, output.size())
                != OrthancPluginErrorCode_Success) {
                OrthancPluginLogError(context_, "orthanc-jxl: Failed to allocate output buffer");
                return OrthancPluginErrorCode_Plugin;
            }

            memcpy(transcoded->data, output.data(), output.size());

            char logMsg[256];
            snprintf(logMsg, sizeof(logMsg),
                "orthanc-jxl: Transcoded FROM JXL %zu KB -> %zu KB",
                jxlData.size() / 1024, pixels.size() / 1024);
            OrthancPluginLogInfo(context_, logMsg);

            return OrthancPluginErrorCode_Success;
        }

        // Case 2: JXL is requested and source is not JXL (TO-JXL)
        if (jxlRequested && !IsJxlTransferSyntax(currentTs)) {
            DicomImageInfo info = handler.GetImageInfo();

            // Get raw pixels
            std::vector<uint8_t> pixels = handler.GetPixelData();

            // Determine pixel format
            PixelFormat format;
            if (info.samplesPerPixel == 1) {
                format = (info.bitsAllocated <= 8) ? PixelFormat::Gray8 : PixelFormat::Gray16;
            } else {
                format = (info.bitsAllocated <= 8) ? PixelFormat::RGB24 : PixelFormat::RGB48;
            }

            // Encode to JXL (progressive lossless with center-first ordering)
            std::vector<uint8_t> jxlData = JxlCodec::EncodeProgressiveLossless(
                pixels.data(),
                info.width,
                info.height,
                format,
                7,  // effort
                static_cast<int>(info.width / 2),   // centerX
                static_cast<int>(info.height / 2)   // centerY
            );

            // Set JXL pixel data in DICOM
            handler.SetJxlPixelData(jxlData);
            handler.SetTransferSyntax(TS_JPEG_XL_LOSSLESS);

            // Write to buffer
            std::vector<uint8_t> output = handler.WriteToBuffer(TS_JPEG_XL_LOSSLESS);

            // Allocate Orthanc buffer
            if (OrthancPluginCreateMemoryBuffer(context_, transcoded, output.size())
                != OrthancPluginErrorCode_Success) {
                OrthancPluginLogError(context_, "orthanc-jxl: Failed to allocate output buffer");
                return OrthancPluginErrorCode_Plugin;
            }

            memcpy(transcoded->data, output.data(), output.size());

            // Log compression stats
            double ratio = static_cast<double>(pixels.size()) / jxlData.size();
            char logMsg[256];
            snprintf(logMsg, sizeof(logMsg),
                "orthanc-jxl: Transcoded TO JXL %zu KB -> %zu KB (%.2fx)",
                pixels.size() / 1024, jxlData.size() / 1024, ratio);
            OrthancPluginLogInfo(context_, logMsg);

            return OrthancPluginErrorCode_Success;
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
