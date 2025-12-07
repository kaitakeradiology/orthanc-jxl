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
    bool isSigned = false;
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
    std::vector<uint8_t> GetPixelData() const;           // For uncompressed
    std::vector<uint8_t> GetEncapsulatedData(uint32_t frameIndex = 0) const;  // For compressed

    // Modification
    void SetJxlPixelData(const std::vector<uint8_t>& jxlData);
    void SetJxlPixelData(const uint8_t* data, size_t size);
    void SetTransferSyntax(const std::string& transferSyntaxUid);

    // Serialization
    std::vector<uint8_t> WriteToBuffer(const std::string& transferSyntaxUid) const;

private:
    std::unique_ptr<DcmFileFormat> fileFormat_;
    bool parseWarning_ = false;
};

} // namespace orthanc_jxl
