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

#include <string_view>

namespace orthanc_jxl {

// Official DICOM Transfer Syntax UIDs for JPEG-XL (PS3.5 2025e)
constexpr const char* TS_JPEG_XL_LOSSLESS = "1.2.840.10008.1.2.4.110";
constexpr const char* TS_JPEG_XL_JPEG_RECOMPRESSION = "1.2.840.10008.1.2.4.111";
constexpr const char* TS_JPEG_XL = "1.2.840.10008.1.2.4.112";

inline bool IsJxlTransferSyntax(std::string_view ts) {
    return ts == TS_JPEG_XL_LOSSLESS ||
           ts == TS_JPEG_XL_JPEG_RECOMPRESSION ||
           ts == TS_JPEG_XL;
}

} // namespace orthanc_jxl
