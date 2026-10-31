#!/usr/bin/env python3
"""Bundle a user-supplied FiveM RPF into the release ASI as a PE overlay.

Usage:
  python tools/bundle_motion_blur.py BetterVideoExport.asi zz_VYNE_MotionBlur.rpf BetterVideoExport-MotionBlur.asi

The runtime reads a 16-byte footer (magic, payload size, CRC32), then installs
that RPF to FiveM.app/mods on launch. Windows ignores this PE overlay.
"""
import argparse
import binascii
import pathlib
import struct

MAGIC = b"BVEMB01!"
MAX_PAYLOAD = 2 * 1024 * 1024

def bundle(asi: pathlib.Path, rpf: pathlib.Path, output: pathlib.Path) -> None:
    image = asi.read_bytes()
    preset = rpf.read_bytes()
    if not preset.startswith(b"RPF7") or len(preset) > MAX_PAYLOAD:
        raise ValueError("Expected a valid-sized GTA V RPF7 archive")
    # Support re-bundling without accumulating stale overlays.
    if len(image) >= 16 and image[-16:-8] == MAGIC:
        size, crc = struct.unpack("<II", image[-8:])
        start = len(image) - 16 - size
        if start >= 0 and binascii.crc32(image[start:-16]) == crc:
            image = image[:start]
    if not image.startswith(b"MZ"):
        raise ValueError("Input does not appear to be a Windows ASI/DLL")
    footer = MAGIC + struct.pack("<II", len(preset), binascii.crc32(preset))
    output.write_bytes(image + preset + footer)
    print(f"Bundled {len(preset)} RPF bytes into {output}")

if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("asi", type=pathlib.Path)
    p.add_argument("rpf", type=pathlib.Path)
    p.add_argument("output", type=pathlib.Path)
    a = p.parse_args()
    bundle(a.asi, a.rpf, a.output)
