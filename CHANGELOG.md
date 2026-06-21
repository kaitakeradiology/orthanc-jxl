# Changelog

All notable changes to this project are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project adheres
to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.3.0] - 2026-06-21

This release makes multi-frame transcoding correct and parallel, fixes several
data-integrity bugs in the encode path, and makes the build self-contained by
fetching the Orthanc plugin SDK header automatically.

**Minimum supported Orthanc is now 1.12.10** (the release that introduced the
JPEG-XL transfer syntaxes).

### Fixed

- **Multi-frame data loss.** The transcoder only ever handled frame 0, so
  multi-frame instances silently lost every frame but the first. Each frame is now
  encoded into its own encapsulated fragment end-to-end.
- **Lossy mislabeled as lossless.** Output produced with distance > 0 is now tagged
  with the lossy transfer syntax (`.112`) instead of the lossless one (`.110`).
- **Out-of-bounds read on malformed input.** Pixel-buffer sizes are validated before
  use, guarding against truncated or malformed instances.
- **Planar / YBR / big-endian handling.** Planar source is converted to interleaved
  (and `PlanarConfiguration` set to 0); YBR and big-endian instances now round-trip
  byte-exact.
- **Identity passthrough.** A stored JXL instance served back in its own transfer
  syntax no longer triggers Orthanc's "Unsupported transfer syntax" error.
- **Honest color metadata.** Color encoding now uses the correct per-format transfer
  function instead of asserting linear sRGB on raw samples.

### Added

- **Frame-level parallelism.** Encoding and decoding run across a shared worker pool,
  so a single multi-frame instance can saturate all cores.
- **Persistent thread pool** with thread-local libjxl runners, eliminating
  per-call pool construction/teardown.
- **`OrthancJxl.EncodeThreads`** configuration option to tune ingest concurrency.
- **End-to-end roundtrip test** (`jxl-roundtrip`), verifying byte-exact transcoding
  across multi-frame, planar, YBR, big-endian, and odd-dimension fixtures, wired into
  `meson test`.
- **Throughput benchmark** (`jxl-throughput`) for measuring encode/decode performance.
- **Automatic Orthanc SDK header resolution.** `OrthancCPlugin.h` is now resolved at
  configure time: an installed system header is used if present, otherwise the pinned
  1.12.10 header is downloaded and SHA-256 verified. Use
  `-Dorthanc_sdk_include=<dir>` to build offline or against a custom SDK.

### Changed

- Build no longer hardcodes `-I/usr/include/orthanc`; the SDK include path is resolved
  as described above.
- Documentation updated to reflect the 1.12.10 minimum and the SDK fetch.

[0.3.0]: https://github.com/kaitakeradiology/orthanc-jxl/releases/tag/v0.3.0
