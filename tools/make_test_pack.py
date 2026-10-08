#!/usr/bin/env python3
"""Writes a small, valid template pack (doc 20) for smoke tests.

    tools/make_test_pack.py OUT.zip [--version 1.0.0] [--template lower-third-two-lines]

One built-in template from drop-ins/titles/data/templates, a generated
preview (a plain 16x9 PNG), and pack.xml with each file's size and SHA-256.
The repository holds no binary files, so tests make their packs.

Check it with `u-studio-titles --install-pack OUT.zip` (exit status 0,
"installed test/smoke-pack <version> in ...").
"""
import argparse
import hashlib
import os
import struct
import sys
import zipfile
import zlib
from xml.sax.saxutils import escape, quoteattr

HERE = os.path.dirname(os.path.abspath(__file__))
TEMPLATES = os.path.join(HERE, "..", "drop-ins", "titles", "data", "templates")


def png(width=16, height=9, rgb=(27, 18, 48)):
    """A plain PNG: a real picture, so the pack's type checks pass."""
    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)
    rows = b"".join(b"\x00" + bytes(rgb) * width for _ in range(height))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("out")
    parser.add_argument("--version", default="1.0.0")
    parser.add_argument("--template", default="lower-third-two-lines")
    args = parser.parse_args()
    with open(os.path.join(TEMPLATES, args.template + ".ustitle"), "rb") as f:
        template = f.read()
    files = {
        f"templates/{args.template}.ustitle": template,
        f"previews/{args.template}.png": png(),
    }
    manifest = (f'<?xml version="1.0" encoding="UTF-8"?>\n'
                f'<pack format="1" id="test/smoke-pack" version={quoteattr(args.version)}>\n'
                f"  <title>Smoke test pack</title>\n  <author>U-Stu tests</author>\n  <licence>CC0-1.0</licence>\n")
    for path, data in files.items():
        manifest += (f"  <file path={quoteattr(path)} size=\"{len(data)}\" "
                     f"sha256=\"{hashlib.sha256(data).hexdigest()}\"/>\n")
    manifest += "</pack>\n"
    with zipfile.ZipFile(args.out, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("pack.xml", manifest)
        for path, data in files.items():
            z.writestr(path, data)
    print(f"wrote {args.out} ({escape(args.template)}, version {args.version})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
