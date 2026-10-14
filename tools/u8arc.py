"""Reader for Nintendo U8 archives (magic 0x55AA382D). Generic.

Format (source: wiki.tockdom.com "U8 (File Format)"; this module does not depend on Dolphin's
`Source/Core/DiscIO`):
  0x20-byte header: magic, offset of the first node, size of nodes + names, offset of the data
  12-byte nodes: [type(1) | name offset(3)] [data | parent] [size | end index]
  node 0 is the root; its 3rd field = total number of nodes.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass

MAGIC = 0x55AA382D


@dataclass
class U8Entry:
    path: str
    offset: int    # relative to the start of the archive
    size: int


def parse(data: bytes) -> list[U8Entry]:
    magic, root, _hdr_size, _data_off = struct.unpack(">IIII", data[:16])
    if magic != MAGIC:
        raise ValueError(f"not a U8 archive (magic {magic:#x})")
    count = struct.unpack(">I", data[root + 8:root + 12])[0]
    names_base = root + count * 12

    def name_at(off: int) -> str:
        start = names_base + off
        return data[start:data.index(b"\0", start)].decode("shift_jis", "replace")

    out: list[U8Entry] = []
    stack: list[tuple[int, str]] = [(count, "")]
    for i in range(1, count):
        while stack and i >= stack[-1][0]:
            stack.pop()
        word0, a, b = struct.unpack(">III", data[root + i * 12:root + i * 12 + 12])
        name = name_at(word0 & 0xFFFFFF)
        parent = stack[-1][1]
        path = f"{parent}/{name}" if parent else name
        if word0 >> 24:
            stack.append((b, path))
        else:
            out.append(U8Entry(path, a, b))
    return out
