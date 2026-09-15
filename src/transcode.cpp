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

#include "transcode.h"

#include "dicom_handler.h"
#include "jxl_codec.h"
#include "pixel_layout.h"
#include "transfer_syntax.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace orthanc_jxl {

namespace {

// Pick the JXL PixelFormat for the given DICOM geometry.
PixelFormat PixelFormatFor(const DicomImageInfo& info) {
    if (info.samplesPerPixel == 1) {
        return (info.bitsAllocated <= 8) ? PixelFormat::Gray8 : PixelFormat::Gray16;
    }
    return (info.bitsAllocated <= 8) ? PixelFormat::RGB24 : PixelFormat::RGB48;
}

// Format a double as a DICOM DS (Decimal String, <= 16 characters). %.6g
// comfortably covers RescaleIntercept/Slope and compression-ratio magnitudes
// without the excess digits std::to_string would produce.
std::string FormatDS(double value) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6g", value);
    return std::string(buf);
}

// PS3.3 C.7.6.1.1.5: "the first value ... shall be DERIVED ... The remaining
// values are unaffected". Absent ImageType defaults to ORIGINAL\PRIMARY
// per PS3.3 C.7.6.1.1.2, so an absent tag here becomes DERIVED\SECONDARY.
std::string DerivedImageType(const std::string& existing) {
    if (existing.empty()) {
        return "DERIVED\\SECONDARY";
    }
    size_t pos = existing.find('\\');
    std::string rest = (pos == std::string::npos) ? std::string() : existing.substr(pos);
    return "DERIVED" + rest;
}

// Apply the PS3.3 C.7.6.1.1.5 lossy-compression tags plus, for signed input,
// the tag-side half of the sign-offset rewrite whose pixel-side half already
// ran (in TranscodeToJxl, before encoding) via ApplySignedOffset. Common to
// every lossy TO-JXL encode; the signed-only steps are gated on
// applySignedOffset.
//
// The offset is series-CONSTANT by construction: it depends only on
// BitsStored, which does not vary between instances of one series, so a
// consumer (hikaru-core) that reads the intercept from a series' first
// instance and applies it to every instance in that series stays correct.
void ApplyLossyTags(DicomHandler& handler, bool applySignedOffset,
                    uint32_t signedOffset, size_t nativeBytes, size_t encodedBytes) {
    handler.SetString(0x0028, 0x2110, "01");  // LossyImageCompression

    const double ratio = encodedBytes
        ? static_cast<double>(nativeBytes) / static_cast<double>(encodedBytes) : 0.0;
    char ratioBuf[32];
    std::snprintf(ratioBuf, sizeof(ratioBuf), "%.2f", ratio);
    handler.SetString(0x0028, 0x2112, ratioBuf);  // LossyImageCompressionRatio

    // Verified defined term for JPEG XL (PS3.3 Table C.7-11a, Lossy Image
    // Compression Method).
    handler.SetString(0x0028, 0x2114, "ISO_18181_1");  // LossyImageCompressionMethod

    std::string imageType;
    handler.GetString(0x0008, 0x0008, imageType);  // ok if absent - empty string
    handler.SetString(0x0008, 0x0008, DerivedImageType(imageType));

    // A lossy re-encode is a derived image, not the original acquisition -
    // assign it a fresh identity rather than let JXL-compressed bits
    // masquerade under the source SOPInstanceUID. (plugin.cpp already
    // declined this transcode if Orthanc's allowNewSopInstanceUid forbade a
    // new identity.)
    handler.GenerateNewSopInstanceUid();

    if (!applySignedOffset) {
        return;  // Unsigned input: no offset was applied, no LUT rewrite needed.
    }

    // --- Undo the signed -> unsigned pixel bias via the Modality LUT -------
    handler.SetUint16(0x0028, 0x0103, 0);  // PixelRepresentation -> unsigned

    std::string interceptStr;
    const double existingIntercept = handler.GetString(0x0028, 0x1052, interceptStr)
        ? std::atof(interceptStr.c_str()) : 0.0;
    handler.SetString(0x0028, 0x1052,
        FormatDS(existingIntercept - static_cast<double>(signedOffset)));  // RescaleIntercept

    std::string slopeStr;
    if (!handler.GetString(0x0028, 0x1053, slopeStr)) {
        handler.SetString(0x0028, 0x1053, "1");  // RescaleSlope, if absent
    }

    // PixelPaddingValue/RangeLimit are genuine signed integers (VR SS, not
    // the raw BitsStored-bit-packed encoding pixel data uses), so a plain
    // arithmetic offset - not the bit-level ApplySignedOffset transform - is
    // the correct conversion. Re-typed to US since PixelRepresentation is now 0.
    int16_t paddingValue = 0;
    if (handler.GetSint16(0x0028, 0x0120, paddingValue)) {
        handler.SetUint16(0x0028, 0x0120,
            static_cast<uint16_t>(static_cast<int32_t>(paddingValue) +
                                  static_cast<int32_t>(signedOffset)),
            /*forceUnsignedVR=*/true);
    }
    int16_t paddingLimit = 0;
    if (handler.GetSint16(0x0028, 0x0121, paddingLimit)) {
        handler.SetUint16(0x0028, 0x0121,
            static_cast<uint16_t>(static_cast<int32_t>(paddingLimit) +
                                  static_cast<int32_t>(signedOffset)),
            /*forceUnsignedVR=*/true);
    }

    // Smallest/LargestImagePixelValue described the old signed range and are
    // not recomputed here - remove rather than mislead a reader that doesn't
    // apply RescaleIntercept before comparing against them.
    handler.RemoveTag(0x0028, 0x0106);  // SmallestImagePixelValue
    handler.RemoveTag(0x0028, 0x0107);  // LargestImagePixelValue
}

}  // namespace

TranscodeResult TranscodeToJxl(const void* dicom, size_t size,
                               const PluginConfig& config, ThreadPool& pool,
                               int singleFrameThreads) {
    DicomHandler handler(dicom, size);

    // Decode compressed sources (JPEG / JPEG-LS / JPEG2000-if-registered) to
    // native before reading geometry + pixels; no-op for already-uncompressed.
    handler.EnsureUncompressed();

    DicomImageInfo info = handler.GetImageInfo();

    const size_t frameSize = info.FrameSizeBytes();
    const uint32_t frameCount = info.numberOfFrames;
    if (frameSize == 0 || frameCount == 0) {
        throw DicomHandlerError("Invalid image geometry for encoding");
    }

    // Validate the pixel buffer actually holds every frame before slicing it -
    // guards against malformed/truncated instances.
    std::vector<uint8_t> pixels = handler.GetPixelData();
    const size_t expected = static_cast<size_t>(frameCount) * frameSize;
    if (pixels.size() < expected) {
        throw DicomHandlerError("Pixel data smaller than declared geometry");
    }

    const PixelFormat format = PixelFormatFor(info);
    const int channels = JxlCodec::NumChannels(format);
    const int bytesPerSample = JxlCodec::BitsPerSample(format) / 8;
    const bool planar = (info.planarConfiguration == 1 && info.samplesPerPixel > 1);

    EncodeOptions opts = config.GetEncodeOptions(info.width, info.height);
    // This plugin's own convention (not a DICOM requirement, but the one this
    // codebase follows throughout): .110 always carries mathematically
    // lossless bits, .112 always carries the lossy VarDCT output. There is no
    // config key that can override this - GetEncodeOptions/opts above is the
    // only source of the mode/distance decision, so a lossy config always
    // lands on TS_JPEG_XL and never TS_JPEG_XL_LOSSLESS from this function.
    // (plugin.cpp's transcoder callback additionally forces a LOSSLESS
    // ProgressiveLossless re-encode - not reachable here - when the requester
    // only accepts .110 but the plugin is configured for lossy VarDCT, so a
    // lossy stream is never mislabelled as Lossless either.)
    const bool lossy =
        (opts.mode == EncodeMode::ProgressiveVarDCT && opts.distance > 0.0f);
    const std::string outTs = lossy ? TS_JPEG_XL : TS_JPEG_XL_LOSSLESS;

    // For a lossy encode of SIGNED pixel data, bias every sample into
    // unsigned "offset binary" before it reaches libjxl - see
    // SignedSampleToOffsetBinary in pixel_layout.h for why. Never touches a
    // lossless path (bit-exact roundtrip is required there), and is
    // independent of frame slicing since it's a per-sample transform applied
    // to the whole (all-frames-concatenated) pixel buffer up front.
    const bool applySignedOffset = lossy && info.isSigned;
    const uint32_t signedOffset =
        applySignedOffset ? (1u << (info.bitsStored - 1)) : 0u;
    if (applySignedOffset) {
        if (bytesPerSample == 2) {
            ApplySignedOffset<uint16_t>(
                reinterpret_cast<uint16_t*>(pixels.data()),
                pixels.size() / 2, info.bitsStored);
        } else if (bytesPerSample == 1) {
            ApplySignedOffset<uint8_t>(pixels.data(), pixels.size(), info.bitsStored);
        }
    }

    // Frame-parallel: encode each frame single-threaded so the shared pool, not
    // nested libjxl runners, is the sole source of threads. A lone frame falls
    // back to libjxl's own threads (overridable for ingest-concurrency tuning).
    const int frameThreads = (frameCount > 1)
        ? JxlCodec::kSingleThreaded : singleFrameThreads;

    std::vector<std::vector<uint8_t>> encoded(frameCount);
    ParallelFor(pool, frameCount, [&](size_t f) {
        const uint8_t* src = pixels.data() + f * frameSize;
        if (planar) {
            std::vector<uint8_t> interleaved = PlanarToInterleaved(
                src, static_cast<size_t>(info.width) * info.height,
                channels, bytesPerSample);
            encoded[f] = JxlCodec::Encode(interleaved.data(), info.width,
                                          info.height, format, opts, frameThreads);
        } else {
            encoded[f] = JxlCodec::Encode(src, info.width, info.height,
                                          format, opts, frameThreads);
        }
    });

    size_t encodedBytesTotal = 0;
    for (const auto& e : encoded) {
        encodedBytesTotal += e.size();
    }

    handler.SetEncapsulatedFrames(encoded, outTs);
    if (planar) {
        // Encapsulated pixel data is colour-by-pixel by definition.
        handler.SetUint16(0x0028, 0x0006, 0);  // PlanarConfiguration
    }
    handler.SetTransferSyntax(outTs);

    if (lossy) {
        ApplyLossyTags(handler, applySignedOffset, signedOffset, expected, encodedBytesTotal);
    }

    TranscodeResult result;
    result.dicom = handler.WriteToBuffer(outTs);
    result.frameCount = frameCount;
    result.nativeBytes = expected;
    result.encodedBytes = encodedBytesTotal;
    return result;
}

TranscodeResult TranscodeFromJxl(const void* dicom, size_t size,
                                 const std::string& uncompressedTs, ThreadPool& pool) {
    DicomHandler handler(dicom, size);

    uint32_t frameCount = handler.GetEncapsulatedFrameCount();
    if (frameCount == 0) {
        throw DicomHandlerError("JXL pixel data has no frames");
    }

    // Pull every encapsulated frame up front (DCMTK access is not thread-safe),
    // then decode frames in parallel.
    std::vector<std::vector<uint8_t>> jxlFrames(frameCount);
    for (uint32_t f = 0; f < frameCount; ++f) {
        jxlFrames[f] = handler.GetEncapsulatedData(f);
    }

    std::vector<std::vector<uint8_t>> decoded(frameCount);
    const int frameThreads = (frameCount > 1)
        ? JxlCodec::kSingleThreaded : JxlCodec::kDefaultThreads;
    ParallelFor(pool, frameCount, [&](size_t f) {
        decoded[f] = JxlCodec::Decode(jxlFrames[f], frameThreads).first;
    });

    // Concatenate frames into a single native pixel-data blob.
    size_t totalSize = 0;
    for (const auto& d : decoded) {
        totalSize += d.size();
    }
    std::vector<uint8_t> pixels;
    pixels.reserve(totalSize);
    for (auto& d : decoded) {
        pixels.insert(pixels.end(), d.begin(), d.end());
    }

    handler.SetNativePixelData(pixels);
    handler.SetTransferSyntax(uncompressedTs);

    TranscodeResult result;
    result.dicom = handler.WriteToBuffer(uncompressedTs);
    result.frameCount = frameCount;
    result.nativeBytes = pixels.size();
    result.encodedBytes = 0;
    return result;
}

}  // namespace orthanc_jxl
