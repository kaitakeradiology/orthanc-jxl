# orthanc-jxl

JPEG-XL transfer syntax plugin for [Orthanc](https://www.orthanc-server.com/) DICOM server.

## Overview

orthanc-jxl adds native JPEG-XL encoding and decoding support to Orthanc.

The plugin implements the official DICOM Transfer Syntax UIDs defined in PS3.5 2025e.

## Features

- **JPEG-XL Encoding**: Transcode DICOM images to JPEG-XL lossless format
- **JPEG-XL Decoding**: View JPEG-XL encoded DICOM images in Orthanc Explorer

- **Official DICOM Transfer Syntaxes**:
  - `1.2.840.10008.1.2.4.110` - JPEG XL Lossless
  - `1.2.840.10008.1.2.4.111` - JPEG XL JPEG Recompression
  - `1.2.840.10008.1.2.4.112` - JPEG XL (lossy)

- Center-first group ordering for streaming applications
- 8-bit and 16-bit grayscale/RGB pixel formats
- Parallel encoding via libjxl thread runner

## Requirements

**Orthanc with JPEG-XL transfer syntax support** - requires Orthanc revision [e94178d6b6a2](https://orthanc.uclouvain.be/hg/orthanc/rev/e94178d6b6a2) (2025-12-09) or later.

## Building

### Prerequisites

- C++17 compiler (GCC >= 9, Clang >= 10)
- [Meson](https://mesonbuild.com/) build system (>= 0.50)
- [libjxl](https://github.com/libjxl/libjxl) (>= 0.8)
- [DCMTK](https://dicom.offis.de/dcmtk) (>= 3.6.7)
- [Orthanc](https://www.orthanc-server.com/) (>= e94178d6b6a2, see Requirements)

#### Debian/Ubuntu

```bash
sudo apt install meson ninja-build libjxl-dev libdcmtk-dev orthanc-dev
```

#### Fedora

```bash
sudo dnf install meson ninja-build libjxl-devel dcmtk-devel orthanc-devel
```

### Compile

```bash
meson setup build
meson compile -C build
```

### Build Options

```bash
# Debug build
meson setup build --buildtype=debug

```

## Installation

### System Install

```bash
sudo meson install -C build
```

This installs to `$PREFIX/lib/orthanc/plugins/`.

### Manual Install

Copy the plugin to your Orthanc plugins directory:

```bash
sudo cp build/src/libOrthancJxl.so /usr/share/orthanc/plugins/
```

### Verify Installation

Restart Orthanc and check the logs for:

```
orthanc-jxl: Plugin initialized - JPEG-XL transfer syntaxes enabled
```

## Usage

Once installed, the plugin automatically:

1. **Decodes** JPEG-XL encoded DICOM images for viewing in Orthanc Explorer
2. **Encodes** images to JPEG-XL when transcoding is requested with a JXL transfer syntax

### Transcoding via REST API

```bash
# Transcode a study to JPEG-XL lossless
curl -X POST http://localhost:8042/studies/{id}/modify \
  -d '{"Transcode": "1.2.840.10008.1.2.4.110"}'
```

## Encoding Modes

| Mode | Progressive | Lossless | Use Case |
|------|-------------|----------|----------|
| Lossless | No | Yes | Archival storage |
| ProgressiveLossless | Yes | Yes | Streaming + archival |

The plugin defaults to **ProgressiveLossless** mode, matching `cjxl -d 0 -p -e 7 --group_order 1`:
- Effort 7 (balances encode speed and compression)
- Responsive mode with squeeze transform
- Center-first group ordering for streaming decode

## Limitations

- Single-frame images only (multi-frame support planned)
- Lossless encoding only (lossy VarDCT mode planned)
- No configuration options yet (effort level, etc.)

## Contributing

Contributions are welcome! Please open an issue or pull request on GitHub.

## License

GPL-3.0-or-later

This plugin is licensed under GPL-3.0 to comply with the Orthanc Plugin SDK license requirements.

## Dependencies

| Library | License | Purpose |
|---------|---------|---------|
| [libjxl](https://github.com/libjxl/libjxl) | BSD-3-Clause | JPEG-XL codec |
| [DCMTK](https://dicom.offis.de/dcmtk) | BSD-3-Clause | DICOM parsing |
| [Orthanc SDK](https://www.orthanc-server.com/) | GPL-3.0 | Plugin API |

## References

- [DICOM PS3.5 2025e - JPEG-XL Transfer Syntaxes](https://dicom.nema.org/medical/dicom/current/output/html/part05.html)
- [JPEG-XL Reference Implementation](https://github.com/libjxl/libjxl)
- [Orthanc Plugin SDK](https://orthanc.uclouvain.be/book/developers/creating-plugins.html)
