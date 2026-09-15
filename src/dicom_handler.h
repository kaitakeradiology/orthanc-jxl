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
#include <memory>
#include <string>
#include <vector>
#include <stdexcept>

// Forward declarations for DCMTK
class DcmFileFormat;

namespace orthanc_jxl {

class DicomHandlerError : public std::runtime_error {
public:
    explicit DicomHandlerError(const std::string& msg) : std::runtime_error(msg) {}
};

struct DicomImageInfo {
    uint32_t width = 0;
    uint32_t height = 0;
    uint16_t bitsAllocated = 0;
    uint16_t bitsStored = 0;
    uint16_t highBit = 0;
    uint16_t samplesPerPixel = 0;
    uint32_t numberOfFrames = 1;
    uint16_t planarConfiguration = 0;   // 0 = interleaved (R1G1B1...), 1 = planar (R..G..B..)
    bool isSigned = false;
    std::string photometricInterpretation;  // e.g. MONOCHROME2, RGB, YBR_FULL

    // Bytes of a single uncompressed frame.
    size_t FrameSizeBytes() const {
        return static_cast<size_t>(width) * height * samplesPerPixel
               * ((bitsAllocated + 7u) / 8u);
    }
};

class DicomHandler {
public:
    // Constructor parses DICOM from memory
    DicomHandler(const void* data, size_t size);
    ~DicomHandler();

    // Non-copyable, movable
    DicomHandler(const DicomHandler&) = delete;
    DicomHandler& operator=(const DicomHandler&) = delete;
    DicomHandler(DicomHandler&& other) noexcept;
    DicomHandler& operator=(DicomHandler&& other) noexcept;

    // Image metadata
    DicomImageInfo GetImageInfo() const;
    std::string GetTransferSyntax() const;
    bool HadParseWarning() const { return parseWarning_; }

    // Pixel data access
    // Decompress compressed source pixels in place to native (Little Endian
    // Explicit) via DCMTK's registered decoders (the codec registry lives in
    // libdcmdata, shared with Orthanc core, which registers JPEG/JPEG-LS/...).
    // No-op if already native; throws if no decoder is available for the source.
    void EnsureUncompressed();
    std::vector<uint8_t> GetPixelData() const;           // For uncompressed (all frames)
    std::vector<uint8_t> GetEncapsulatedData(uint32_t frameIndex = 0) const;  // For compressed
    uint32_t GetEncapsulatedFrameCount() const;          // Number of encapsulated frames

    // Modification
    // Store one encapsulated fragment per frame under the given JXL transfer syntax.
    void SetEncapsulatedFrames(const std::vector<std::vector<uint8_t>>& frames,
                               const std::string& transferSyntaxUid);
    void SetNativePixelData(const std::vector<uint8_t>& pixelData);
    void SetNativePixelData(const uint8_t* data, size_t size);
    void SetTransferSyntax(const std::string& transferSyntaxUid);

    // Generic tag accessors used by the lossy TO-JXL rewrite (PS3.3
    // C.7.6.1.1.5 lossy tags, RescaleIntercept/Slope, pixel padding).
    //
    // forceUnsignedVR is needed for the ambiguous "US or SS" tags
    // (PixelPaddingValue/RangeLimit): dictionary-default VR resolution for a
    // freshly-created element does not know PixelRepresentation has just
    // changed, so a caller that just converted a value from signed to
    // unsigned must say so explicitly.
    void SetUint16(uint16_t group, uint16_t element, uint16_t value,
                  bool forceUnsignedVR = false);  // e.g. PlanarConfiguration
    void SetSint16(uint16_t group, uint16_t element, int16_t value);  // explicit VR SS
    bool GetSint16(uint16_t group, uint16_t element, int16_t& value) const;
    bool GetUint16(uint16_t group, uint16_t element, uint16_t& value) const;

    // String-valued tags (CS/DS/UI/...). GetString returns false (value left
    // untouched) if the tag is absent - how callers tell "not present" from
    // "present and empty".
    bool GetString(uint16_t group, uint16_t element, std::string& value) const;
    void SetString(uint16_t group, uint16_t element, const std::string& value);

    // Same as GetString but reads the FILE META INFO header (group 0002)
    // instead of the dataset - e.g. MediaStorageSOPInstanceUID.
    bool GetMetaString(uint16_t group, uint16_t element, std::string& value) const;

    void RemoveTag(uint16_t group, uint16_t element);

    // Generate a fresh SOP Instance UID, set it on the dataset, and keep the
    // file meta header's MediaStorageSOPInstanceUID in sync (PS3.10 requires
    // the two to match). Returns the new UID.
    std::string GenerateNewSopInstanceUid();

    // Serialization
    std::vector<uint8_t> WriteToBuffer(const std::string& transferSyntaxUid) const;

private:
    std::unique_ptr<DcmFileFormat> fileFormat_;
    bool parseWarning_ = false;
};

} // namespace orthanc_jxl
