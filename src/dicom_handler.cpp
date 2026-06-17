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

#include "dicom_handler.h"
#include "transfer_syntax.h"

#include <dcmtk/dcmdata/dctk.h>
#include <dcmtk/dcmdata/dcpixel.h>
#include <dcmtk/dcmdata/dcpxitem.h>
#include <dcmtk/dcmdata/dcofsetl.h>
#include <dcmtk/dcmdata/dcuid.h>
#include <dcmtk/dcmdata/dcistrmb.h>
#include <dcmtk/dcmdata/dcostrmb.h>
#include <dcmtk/dcmdata/dcwcache.h>

#include <cstring>

namespace orthanc_jxl {

namespace {

// Map a DICOM transfer syntax UID to the DCMTK enum plus its VR encoding type.
// Throws DicomHandlerError for syntaxes this plugin does not write.
E_TransferSyntax MapTransferSyntax(const std::string& uid, E_EncodingType& encType) {
    encType = EET_ExplicitLength;
    if (uid == TS_JPEG_XL_LOSSLESS)              return EXS_JPEGXLLossless;
    if (uid == TS_JPEG_XL_JPEG_RECOMPRESSION)    return EXS_JPEGXLJPEGRecompression;
    if (uid == TS_JPEG_XL)                       return EXS_JPEGXL;
    if (uid == TS_LITTLE_ENDIAN_EXPLICIT)        return EXS_LittleEndianExplicit;
    if (uid == TS_BIG_ENDIAN_EXPLICIT)           return EXS_BigEndianExplicit;
    if (uid == TS_LITTLE_ENDIAN_IMPLICIT) {
        encType = EET_UndefinedLength;  // Implicit VR uses undefined length
        return EXS_LittleEndianImplicit;
    }
    throw DicomHandlerError("Unsupported transfer syntax: " + uid);
}

}  // namespace

// ============================================================================
// Constructor / Destructor
// ============================================================================

DicomHandler::DicomHandler(const void* data, size_t size) {
    if (!data || size == 0) {
        throw DicomHandlerError("Invalid input DICOM data");
    }

    // Enable lenient parsing for slightly malformed DICOM files
    dcmIgnoreParsingErrors.set(OFTrue);

    // Load from memory buffer
    DcmInputBufferStream inputStream;
    inputStream.setBuffer(data, size);
    inputStream.setEos();

    fileFormat_ = std::make_unique<DcmFileFormat>();
    fileFormat_->transferInit();
    OFCondition status = fileFormat_->read(inputStream, EXS_Unknown, EGL_noChange, DCM_MaxReadLength);

    if (status.bad()) {
        parseWarning_ = true;
        // Continue with lenient mode - don't throw
    }

    fileFormat_->loadAllDataIntoMemory();
    fileFormat_->transferEnd();

    DcmDataset* dataset = fileFormat_->getDataset();
    if (!dataset) {
        throw DicomHandlerError("No dataset in DICOM file");
    }
}

DicomHandler::~DicomHandler() = default;

DicomHandler::DicomHandler(DicomHandler&& other) noexcept
    : fileFormat_(std::move(other.fileFormat_))
    , parseWarning_(other.parseWarning_) {
}

DicomHandler& DicomHandler::operator=(DicomHandler&& other) noexcept {
    if (this != &other) {
        fileFormat_ = std::move(other.fileFormat_);
        parseWarning_ = other.parseWarning_;
    }
    return *this;
}

// ============================================================================
// Image Metadata
// ============================================================================

DicomImageInfo DicomHandler::GetImageInfo() const {
    DcmDataset* dataset = fileFormat_->getDataset();

    DicomImageInfo info;
    Uint16 rows = 0, cols = 0;
    Uint16 bits = 0, stored = 0, high = 0, spp = 0;
    Uint16 pixelRep = 0, planar = 0;

    dataset->findAndGetUint16(DCM_Rows, rows);
    dataset->findAndGetUint16(DCM_Columns, cols);
    dataset->findAndGetUint16(DCM_BitsAllocated, bits);
    dataset->findAndGetUint16(DCM_BitsStored, stored);
    dataset->findAndGetUint16(DCM_HighBit, high);
    dataset->findAndGetUint16(DCM_SamplesPerPixel, spp);
    dataset->findAndGetUint16(DCM_PixelRepresentation, pixelRep);
    dataset->findAndGetUint16(DCM_PlanarConfiguration, planar);

    info.width = cols;
    info.height = rows;
    info.bitsAllocated = bits;
    info.bitsStored = stored;
    info.highBit = high;
    info.samplesPerPixel = spp;
    info.isSigned = (pixelRep != 0);
    info.planarConfiguration = planar;

    // NumberOfFrames is type IS (string); default to 1 when absent.
    Sint32 frames = 0;
    if (dataset->findAndGetSint32(DCM_NumberOfFrames, frames).good() && frames > 0) {
        info.numberOfFrames = static_cast<uint32_t>(frames);
    }

    OFString photometric;
    if (dataset->findAndGetOFString(DCM_PhotometricInterpretation, photometric).good()) {
        info.photometricInterpretation = std::string(photometric.c_str());
        // Trim trailing whitespace/padding that DICOM CS values may carry.
        while (!info.photometricInterpretation.empty() &&
               (info.photometricInterpretation.back() == ' ' ||
                info.photometricInterpretation.back() == '\0')) {
            info.photometricInterpretation.pop_back();
        }
    }

    return info;
}

std::string DicomHandler::GetTransferSyntax() const {
    DcmMetaInfo* metaInfo = fileFormat_->getMetaInfo();
    if (!metaInfo) {
        throw DicomHandlerError("No meta info in DICOM file");
    }

    OFString tsUid;
    OFCondition status = metaInfo->findAndGetOFString(DCM_TransferSyntaxUID, tsUid);
    if (status.bad()) {
        throw DicomHandlerError("Failed to get transfer syntax UID");
    }

    return std::string(tsUid.c_str());
}

// ============================================================================
// Pixel Data Access
// ============================================================================

std::vector<uint8_t> DicomHandler::GetPixelData() const {
    DcmDataset* dataset = fileFormat_->getDataset();

    DcmElement* pixelElement = nullptr;
    OFCondition status = dataset->findAndGetElement(DCM_PixelData, pixelElement);
    if (status.bad() || !pixelElement) {
        throw DicomHandlerError("No pixel data found in DICOM file");
    }

    Uint8* rawData = nullptr;
    status = pixelElement->getUint8Array(rawData);
    if (status.bad() || !rawData) {
        throw DicomHandlerError("Failed to get pixel data array");
    }

    size_t dataSize = pixelElement->getLength();
    return std::vector<uint8_t>(rawData, rawData + dataSize);
}

std::vector<uint8_t> DicomHandler::GetEncapsulatedData(uint32_t frameIndex) const {
    DcmDataset* dataset = fileFormat_->getDataset();

    DcmElement* pixelElement = nullptr;
    OFCondition status = dataset->findAndGetElement(DCM_PixelData, pixelElement);
    if (status.bad() || !pixelElement) {
        throw DicomHandlerError("No pixel data found in DICOM file");
    }

    DcmPixelData* pixelData = OFstatic_cast(DcmPixelData*, pixelElement);

    // Get the original (compressed) representation
    E_TransferSyntax xferSyntax = EXS_Unknown;
    const DcmRepresentationParameter* repParam = nullptr;
    pixelData->getOriginalRepresentationKey(xferSyntax, repParam);

    DcmPixelSequence* pixelSequence = nullptr;
    status = pixelData->getEncapsulatedRepresentation(xferSyntax, repParam, pixelSequence);
    if (status.bad() || !pixelSequence) {
        throw DicomHandlerError("Failed to get encapsulated pixel data");
    }

    // Get the frame data (skip offset table at index 0)
    DcmPixelItem* pixelItem = nullptr;
    status = pixelSequence->getItem(pixelItem, frameIndex + 1);  // +1 to skip offset table
    if (status.bad() || !pixelItem) {
        throw DicomHandlerError("Failed to get pixel item for frame");
    }

    Uint8* fragmentData = nullptr;
    status = pixelItem->getUint8Array(fragmentData);
    if (status.bad() || !fragmentData) {
        throw DicomHandlerError("Failed to get fragment data");
    }

    Uint32 fragmentLength = pixelItem->getLength();
    if (fragmentLength == 0) {
        throw DicomHandlerError("Empty fragment data");
    }

    return std::vector<uint8_t>(fragmentData, fragmentData + fragmentLength);
}

uint32_t DicomHandler::GetEncapsulatedFrameCount() const {
    DcmDataset* dataset = fileFormat_->getDataset();

    DcmElement* pixelElement = nullptr;
    if (dataset->findAndGetElement(DCM_PixelData, pixelElement).bad() || !pixelElement) {
        throw DicomHandlerError("No pixel data found in DICOM file");
    }

    DcmPixelData* pixelData = OFstatic_cast(DcmPixelData*, pixelElement);

    E_TransferSyntax xferSyntax = EXS_Unknown;
    const DcmRepresentationParameter* repParam = nullptr;
    pixelData->getOriginalRepresentationKey(xferSyntax, repParam);

    DcmPixelSequence* pixelSequence = nullptr;
    if (pixelData->getEncapsulatedRepresentation(xferSyntax, repParam, pixelSequence).bad()
        || !pixelSequence) {
        throw DicomHandlerError("Failed to get encapsulated pixel data");
    }

    // We always write exactly one fragment per frame, so the frame count is the
    // number of items minus the Basic Offset Table (item 0).
    unsigned long card = pixelSequence->card();
    return card > 0 ? static_cast<uint32_t>(card - 1) : 0;
}

// ============================================================================
// Modification
// ============================================================================

void DicomHandler::SetEncapsulatedFrames(const std::vector<std::vector<uint8_t>>& frames,
                                         const std::string& transferSyntaxUid) {
    if (frames.empty()) {
        throw DicomHandlerError("No frames to store");
    }

    E_EncodingType encType = EET_ExplicitLength;
    E_TransferSyntax xfer = MapTransferSyntax(transferSyntaxUid, encType);
    if (!IsJxlTransferSyntax(transferSyntaxUid)) {
        throw DicomHandlerError("SetEncapsulatedFrames requires a JXL transfer syntax");
    }

    DcmDataset* dataset = fileFormat_->getDataset();

    // Remove existing pixel data
    delete dataset->remove(DCM_PixelData);

    auto pixelData = std::make_unique<DcmPixelData>(DCM_PixelData);
    auto pixelSequence = std::make_unique<DcmPixelSequence>(DCM_PixelSequenceTag);

    // DICOM PS3.5 Annex A.4 requires a Basic Offset Table as the first item.
    // An empty BOT is valid; with one fragment per frame, frame i maps to item i+1.
    pixelSequence->insert(new DcmPixelItem(DCM_PixelItemTag));

    for (const auto& frame : frames) {
        if (frame.empty()) {
            throw DicomHandlerError("Empty encoded frame");
        }
        auto fragment = std::make_unique<DcmPixelItem>(DCM_PixelItemTag);
        OFCondition status = fragment->putUint8Array(
            frame.data(), static_cast<unsigned long>(frame.size()));
        if (status.bad()) {
            throw DicomHandlerError("Failed to store encoded data in fragment");
        }
        pixelSequence->insert(fragment.release());  // Sequence takes ownership
    }

    // Transfer ownership of the sequence to the pixel data element.
    pixelData->putOriginalRepresentation(xfer, nullptr, pixelSequence.release());

    DcmPixelData* rawPixelData = pixelData.release();
    OFCondition status = dataset->insert(rawPixelData, OFTrue /* replaceOld */);
    if (status.bad()) {
        delete rawPixelData;
        throw DicomHandlerError("Failed to insert pixel data into dataset");
    }
}

void DicomHandler::SetUint16(uint16_t group, uint16_t element, uint16_t value) {
    DcmDataset* dataset = fileFormat_->getDataset();
    OFCondition status = dataset->putAndInsertUint16(DcmTagKey(group, element), value);
    if (status.bad()) {
        throw DicomHandlerError("Failed to set DICOM tag");
    }
}

void DicomHandler::SetNativePixelData(const std::vector<uint8_t>& pixelData) {
    SetNativePixelData(pixelData.data(), pixelData.size());
}

void DicomHandler::SetNativePixelData(const uint8_t* data, size_t size) {
    if (!data || size == 0) {
        throw DicomHandlerError("Invalid pixel data");
    }

    DcmDataset* dataset = fileFormat_->getDataset();

    // Remove existing pixel data
    delete dataset->remove(DCM_PixelData);

    // Create new native (uncompressed) pixel data element
    DcmElement* pixelElement = new DcmOtherByteOtherWord(DCM_PixelData);
    OFCondition status = pixelElement->putUint8Array(data, static_cast<unsigned long>(size));
    if (status.bad()) {
        delete pixelElement;
        throw DicomHandlerError("Failed to store pixel data");
    }

    // Insert into dataset
    status = dataset->insert(pixelElement);
    if (status.bad()) {
        delete pixelElement;
        throw DicomHandlerError("Failed to insert pixel data into dataset");
    }
}

void DicomHandler::SetTransferSyntax(const std::string& transferSyntaxUid) {
    DcmMetaInfo* metaInfo = fileFormat_->getMetaInfo();
    if (metaInfo) {
        OFCondition status = metaInfo->putAndInsertString(
            DCM_TransferSyntaxUID, transferSyntaxUid.c_str());
        if (status.bad()) {
            throw DicomHandlerError("Failed to set transfer syntax UID");
        }
    }
}

// ============================================================================
// Serialization
// ============================================================================

std::vector<uint8_t> DicomHandler::WriteToBuffer(const std::string& transferSyntaxUid) const {
    // Determine transfer syntax
    E_EncodingType encType = EET_ExplicitLength;
    E_TransferSyntax xfer = MapTransferSyntax(transferSyntaxUid, encType);

    DcmDataset* dataset = fileFormat_->getDataset();

    // For compressed syntaxes, choose the correct pixel data representation
    if (IsJxlTransferSyntax(transferSyntaxUid)) {
        DcmElement* pixelElement = nullptr;
        OFCondition status = dataset->findAndGetElement(DCM_PixelData, pixelElement);
        if (status.good() && pixelElement) {
            DcmPixelData* pixelData = OFstatic_cast(DcmPixelData*, pixelElement);
            DcmStack stack;
            pixelData->chooseRepresentation(xfer, nullptr, stack);
            pixelData->removeAllButCurrentRepresentations();
        }
    }

    // Calculate buffer size
    size_t bufferSize = dataset->calcElementLength(xfer, encType) + 4096;

    std::vector<uint8_t> buffer(bufferSize);
    DcmOutputBufferStream outputStream(buffer.data(), bufferSize);

    fileFormat_->transferInit();
    OFCondition status = fileFormat_->write(outputStream, xfer, encType, nullptr);
    fileFormat_->transferEnd();

    if (status.bad()) {
        throw DicomHandlerError(std::string("Failed to write DICOM: ") + status.text());
    }

    // Resize to actual written size
    offile_off_t writtenSize = outputStream.tell();
    buffer.resize(static_cast<size_t>(writtenSize));

    return buffer;
}

} // namespace orthanc_jxl
