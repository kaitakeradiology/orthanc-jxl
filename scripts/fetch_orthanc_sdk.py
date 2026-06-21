#!/usr/bin/env python3
"""Fetch the Orthanc plugin SDK header (OrthancCPlugin.h) at a pinned version.

The Orthanc project does not publish the plugin SDK as a standalone archive, so
this downloads the single raw header from the tagged Mercurial revision and
verifies it against a known SHA-256. It is invoked at meson configure time and is
idempotent: if the destination already holds the expected bytes, nothing is
downloaded (so offline reconfigures and air-gapped builds keep working once the
header is cached under subprojects/).
"""

import argparse
import hashlib
import os
import sys
import tempfile
import urllib.request

# Pinned defaults. Keep VERSION and SHA256 in lockstep when bumping the SDK.
DEFAULT_VERSION = "1.12.10"
DEFAULT_SHA256 = "c3ec6e6efa3fa7c9e186f0364b9a1fdae098d0c463a592bfeaec14614584a313"

URL_TEMPLATE = (
    "https://orthanc.uclouvain.be/hg/orthanc/raw-file/"
    "Orthanc-{version}/OrthancServer/Plugins/Include/orthanc/OrthancCPlugin.h"
)


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", default=DEFAULT_VERSION,
                        help="Orthanc release tag to fetch the header from")
    parser.add_argument("--sha256", default=DEFAULT_SHA256,
                        help="expected SHA-256 of OrthancCPlugin.h")
    parser.add_argument("--dest", required=True,
                        help="SDK root; header is written to <dest>/orthanc/OrthancCPlugin.h")
    args = parser.parse_args()

    header_path = os.path.join(args.dest, "orthanc", "OrthancCPlugin.h")

    # Idempotent: already cached with the expected contents.
    if os.path.exists(header_path) and sha256_of(header_path) == args.sha256:
        print(header_path)
        return 0

    url = URL_TEMPLATE.format(version=args.version)
    os.makedirs(os.path.dirname(header_path), exist_ok=True)

    try:
        with urllib.request.urlopen(url, timeout=60) as resp:
            data = resp.read()
    except Exception as exc:  # noqa: BLE001 - surface any download failure to meson
        print(f"error: failed to download Orthanc SDK header from {url}: {exc}",
              file=sys.stderr)
        return 1

    actual = hashlib.sha256(data).hexdigest()
    if actual != args.sha256:
        print(f"error: SHA-256 mismatch for {url}\n"
              f"  expected {args.sha256}\n  got      {actual}",
              file=sys.stderr)
        return 1

    # Atomic write so a concurrent/interrupted configure can't leave a partial header.
    fd, tmp = tempfile.mkstemp(dir=os.path.dirname(header_path))
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data)
        os.replace(tmp, header_path)
    except Exception:
        os.unlink(tmp)
        raise

    print(header_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
