# orthanc-jxl

JPEG-XL transfer syntax plugin for [Orthanc](https://www.orthanc-server.com/) DICOM server.

## Overview

orthanc-jxl adds native JPEG-XL encoding and decoding support to Orthanc.

The plugin implements the official DICOM Transfer Syntax UIDs defined in PS3.5 2025e.

## Features

- **JPEG-XL Encoding**: Transcode DICOM images to JPEG-XL, lossless or lossy
- **JPEG-XL Decoding**: View JPEG-XL encoded DICOM images in Orthanc Explorer

- **Official DICOM Transfer Syntaxes**:
  - `1.2.840.10008.1.2.4.110` - JPEG XL Lossless
  - `1.2.840.10008.1.2.4.111` - JPEG XL JPEG Recompression
  - `1.2.840.10008.1.2.4.112` - JPEG XL (lossy)

- Center-first group ordering for streaming applications
- 8-bit and 16-bit grayscale/RGB pixel formats
- Multi-frame instances (one encapsulated fragment per frame)
- Planar (PlanarConfiguration 1) and big-endian source normalization
- Frame-level parallel encoding/decoding across a shared worker pool

## Requirements

**Orthanc with JPEG-XL transfer syntax support** - requires Orthanc **1.12.10** or later, the release that introduced the JPEG-XL transfer syntaxes.

## Building

### Prerequisites

- C++17 compiler (GCC >= 9, Clang >= 10)
- [Meson](https://mesonbuild.com/) build system (>= 0.50)
- [libjxl](https://github.com/libjxl/libjxl) (>= 0.8)
- [DCMTK](https://dicom.offis.de/dcmtk) (>= 3.6.7)
- [Orthanc](https://www.orthanc-server.com/) (>= 1.12.10) at runtime; see Requirements

The Orthanc plugin SDK header (`OrthancCPlugin.h`) is resolved automatically: an
installed system header is used if present, otherwise the pinned 1.12.10 header is
downloaded (and SHA-256 verified) at `meson setup` time. To build fully offline or
against a custom SDK, point meson at a directory containing `orthanc/OrthancCPlugin.h`:

```bash
meson setup build -Dorthanc_sdk_include=/path/to/sdk/include
```

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

## Configuration

Add an `OrthancJxl` section to your Orthanc configuration file:

```json
{
  "OrthancJxl": {
    "Mode": "ProgressiveLossless",
    "Effort": 7,
    "Distance": 0.0,
    "CenterFirstOrdering": true,
    "ProgressiveDC": 0,
    "ProgressiveAC": false
  }
}
```

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `Mode` | string | `"ProgressiveLossless"` | Encoding mode: `"Lossless"`, `"ProgressiveLossless"`, or `"ProgressiveVarDCT"` |
| `Effort` | int | `7` | Encoder effort level (1-10). Higher = slower but better compression |
| `Distance` | float | `0.0` | Quality distance. 0.0 = mathematically lossless |
| `CenterFirstOrdering` | bool | `true` | Enable center-first group ordering for streaming |
| `ProgressiveDC` | int | `0` | VarDCT progressive DC level (0-2) |
| `ProgressiveAC` | bool | `false` | VarDCT progressive AC encoding |

All options are optional. The plugin uses sensible defaults if no configuration is provided.

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
| ProgressiveVarDCT (Distance=0) | Yes | Yes | Lossless, faster decode than modular |
| ProgressiveVarDCT (Distance>0) | Yes | **No** | Lossy - smallest size, preview/streaming |

The plugin defaults to **ProgressiveLossless** mode, matching `cjxl -d 0 -p -e 7 --group_order 1`:
- Effort 7 (balances encode speed and compression)
- Responsive mode with squeeze transform
- Center-first group ordering for streaming decode

### ProgressiveVarDCT + Distance > 0 (lossy)

A lossy encode is always written under transfer syntax `1.2.840.10008.1.2.4.112`
("JPEG XL"), never `.110` ("JPEG XL Lossless") - the plugin's transcoder
callback forces a lossless `ProgressiveLossless` re-encode instead if a
requester only accepts `.110` while the plugin is configured for lossy VarDCT,
so a lossy stream is never mislabelled as lossless.

To make the lossy distance budget count, the encoder enables libjxl's XYB
colour transform (rather than quantising in the raw stored-unit space) and
declares the source's real (nominal) bit depth to libjxl instead of the full
16-bit container `Gray16`/`RGB48` implies (`EncodeOptions::nominalBits`,
`JxlEncoderSetFrameBitDepth`) - without this, a typical CT window is only a
small fraction of a declared-16-bit full scale, so libjxl's perceptual
distance model spends its budget nowhere near where the eye looks and its
distance floor caps achievable quality well above clinically tolerable error.

**Rescaled data (RescaleIntercept present - almost always CT/PET/MR) gets a
13-bit HU codeword** instead of quantising raw stored units:

```
code = round(HU) + 3136,  valid codes 1..8191  (HU -3135..5055)
padding -> code 0
```

Real HU for CT spans roughly -1024 (air) to a few thousand (dense bone/metal);
this range covers it with headroom, in 13 declared bits instead of 16 - a
12-bit codeword would clip dense bone, and re-biasing raw stored HU by a
power-of-two offset (see below) leaves the codeword mismatched to the actual
distribution. Padding pixels (PS3.3 C.7.5.1.1.2: pixels equal to
`PixelPaddingValue`, extended to the inclusive band given by
`PixelPaddingRangeLimit` when present) map to a reserved low-code band
instead of a real HU value, precisely because lossy requantisation cannot
guarantee an exact code for them - PS3.3 gives lossy compression exactly this
mechanism so a decoder can still identify padding afterwards. The lossy
rewrite sets `BitsStored` 13, `HighBit` 12, `PixelRepresentation` 0,
`RescaleIntercept` "-3136", `RescaleSlope` "1", and (when padding was
declared) `PixelPaddingValue` 0 / `PixelPaddingRangeLimit` 63.

Measured on a GE head CT slice at a brain window (jxl-rs decode), RMSE in HU
over non-padding pixels:

| Distance | Size   | RMSE | p99 |
|----------|--------|------|-----|
| 0.2      | 26 KB  | 4.0  | -   |
| 0.5      | 16.7 KB| 5.9  | 16  |
| 1.0      | 12.7 KB| 7.2  | -   |
| lossless | 120 KB | 0    | -   |

Without declaring the real depth, the same brain window was ~0.1% of full
scale to the perceptual model: `d=0.1` -> 12.8 KB at brain-interior RMSE 9.2,
with libjxl's distance floor capping achievable quality near 8 HU regardless
of how far `d` dropped further.

**Data without `RescaleIntercept`** (not rescaled - not typically CT/PET/MR)
keeps the original scheme: for signed pixel data (`PixelRepresentation` 1),
every sample is biased by `2^(BitsStored-1)` into unsigned "offset binary"
before encoding - two's complement puts the padding region immediately next
to small positive values, a huge discontinuity a perceptual codec otherwise
spends its whole budget smearing across real tissue values. The bias is
undone on decode via an adjusted `RescaleIntercept`. `nominalBits` is still
declared as `BitsStored` when narrower than `BitsAllocated`, same as the
HU13 case.

Measured on one 512x512 signed CT (-2000..3622, BitsStored=16), sRGB transfer
function + signed offset, RMSE in stored (non-rescaled) units:

| Distance | Size  | RMSE |
|----------|-------|------|
| 0.25     | ~10KB | 31   |
| 0.5      | ~7KB  | 41   |
| 1.0      | ~5KB  | 76   |

Without the offset/XYB fix, `d=1.0` was 19.9KB at RMSE~147 (about a third of a
soft-tissue window) - `uses_original_profile=TRUE` disabled XYB entirely and
the sign discontinuity ate the distance budget. These numbers are from one
image; treat them as illustrative, not a guarantee for every dataset.

Either way, the lossy rewrite also sets the PS3.3 C.7.6.1.1.5 tags
(`LossyImageCompression`/`Ratio`/`Method`), marks `ImageType` `DERIVED`, and
assigns a new `SOPInstanceUID` (declined via `NotImplemented` if Orthanc's
`allowNewSopInstanceUid` forbids it for that transcode).

libjxl 0.12 (build `7a208214`) also fails the encode outright when
`ProgressiveDC >= 1` and `ProgressiveAC` are both set together with an
explicit (non-auto) group-order centre - which `CenterFirstOrdering=true`
(the default) sets for essentially every image. The plugin clamps
`ProgressiveAC` off automatically in that combination rather than propagate
the failure, and logs a warning once at startup if the configured options
would have hit it.

### Benchmark (512x512 16-bit CT, libjxl 0.12)

```
Mode                     Effort   Enc (ms)   Dec (ms)  Size (KB)    Ratio  Roundtrip
------------------------ ------ ---------- ---------- ---------- -------- ----------
ProgressiveLossless           7       62.8        5.9      113.8    4.50x        yes
ProgressiveLossless           9      183.0       10.7      113.1    4.53x        yes
Lossless                      7       24.6        6.4       92.4    5.54x        yes
Lossless                      9       92.0        6.2       90.2    5.68x        yes
Lossy (d=1.0)                 7       31.1        1.7       20.0   25.65x        N/A
Lossy (d=1.0)                 9       41.7        1.8       20.0   25.63x        N/A
```

The Lossy rows above predate the XYB/signed-offset fix described in
"ProgressiveVarDCT + Distance > 0" and no longer reflect what the plugin
produces (see the RMSE table there instead) - `jxl-benchmark` does not compute
RMSE, only size/timing, so it has not been re-run for this change.

Build with `-Dtests=true` and run `build/tests/jxl-benchmark <dicom_file>`.

## Limitations

- Compressed sources are transcoded to JXL only for transfer syntaxes with a
  registered DCMTK decoder (uncompressed, JPEG, JPEG-LS). JPEG2000 has no DCMTK
  decoder and is not yet supported (would need an OpenJPEG decode path); such
  sources are declined and left in their original transfer syntax.
- Lossy encoding always assigns a new `SOPInstanceUID`; Orthanc transcodes
  that forbid a new identity (`allowNewSopInstanceUid=false`) are declined
  rather than served lossy bits under the original identity.

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
| [nlohmann/json](https://github.com/nlohmann/json) | MIT | Configuration parsing |
| [Orthanc SDK](https://www.orthanc-server.com/) | GPL-3.0 | Plugin API |

## References

- [DICOM PS3.5 2025e - JPEG-XL Transfer Syntaxes](https://dicom.nema.org/medical/dicom/current/output/html/part05.html)
- [JPEG-XL Reference Implementation](https://github.com/libjxl/libjxl)
- [Orthanc Plugin SDK](https://orthanc.uclouvain.be/book/developers/creating-plugins.html)
