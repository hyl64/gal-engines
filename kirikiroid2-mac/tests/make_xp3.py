#!/usr/bin/env python3
"""Build a small XP3 archive, to test krkrpack without game data.

XP3 is the KiriKiri archive format: an 11-byte signature, the index's file
offset, the index (a chain of "File" chunks, each holding "info", "segm" and
"adlr" sub-chunks), then the segments.  This writes the uncompressed variant
(one raw segment per file), which is what the reader must handle first.

    make_xp3.py <out.xp3> <name=path> [<name=path> ...]
"""

import struct
import sys
import zlib

MAGIC = b"XP3\r\n \n\x1a\x8bg\x01"


def chunk(name: bytes, data: bytes) -> bytes:
    return name + struct.pack("<Q", len(data)) + data


def build(entries):
    # The index holds absolute offsets, and its own size does not depend on
    # their values, so a first pass with placeholders sizes it.
    def index_bytes(data_start):
        out = b""
        pos = data_start
        for name, content in entries:
            uname = name.encode("utf-16-le")
            info = struct.pack("<IQQH", 0, len(content), len(content), len(name))
            info += uname
            segm = struct.pack("<IQQQ", 0, pos, len(content), len(content))
            adlr = struct.pack("<I", zlib.adler32(content) & 0xFFFFFFFF)
            body = chunk(b"info", info) + chunk(b"segm", segm) + chunk(b"adlr", adlr)
            out += chunk(b"File", body)
            pos += len(content)
        return out

    placeholder = index_bytes(0)
    index_offset = len(MAGIC) + 8
    # the raw index block is the flag byte, the index's size (u64 LE), the index
    data_start = index_offset + 1 + 8 + len(placeholder)
    index = index_bytes(data_start)
    assert len(index) == len(placeholder)

    blob = bytearray()
    blob += MAGIC
    blob += struct.pack("<Q", index_offset)
    blob += b"\x00"  # the index is not compressed
    blob += struct.pack("<Q", len(index))
    blob += index
    for _, content in entries:
        blob += content
    return bytes(blob)


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    entries = []
    for spec in argv[2:]:
        name, _, path = spec.partition("=")
        with open(path, "rb") as f:
            entries.append((name, f.read()))
    with open(argv[1], "wb") as f:
        f.write(build(entries))
    print(f"wrote {argv[1]}: {len(entries)} file(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
