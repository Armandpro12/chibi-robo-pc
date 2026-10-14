"""Reader for an uncompressed GameCube disc image (.iso / .gcm). Generic.

Layout (source: YAGCD, "Yet Another GameCube Documentation", disc section, and Dolphin
`Source/Core/DiscIO/VolumeGC.cpp`):
  0x0000  boot.bin (0x440 bytes): game ID (6 bytes), disc number, revision, internal name at 0x20
          0x420: offset of the main DOL; 0x424: offset of the FST; 0x428: size of the FST
  0x0440  bi2.bin (0x2000 bytes)
  0x2440  apploader (0x20-byte header: date, entry point, size, trailer size)
FST: 12-byte big-endian entries [type(1) | name offset(3)] [offset | parent] [size | next],
followed by the name table. Entry 0 is the root; its 3rd field = total number of entries.
Compressed images (.rvz, .gcz, .wia) must first be converted to .iso with DolphinTool.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from pathlib import Path


@dataclass
class DiscFile:
    path: str      # path relative to the disc root, "/" separator
    offset: int
    size: int


class GCDisc:
    def __init__(self, path: Path):
        self.path = Path(path)
        self._f = self.path.open("rb")
        boot = self.read(0, 0x440)
        self.game_id = boot[0:6].decode("ascii")
        self.disc_number = boot[6]
        self.revision = boot[7]
        self.internal_name = boot[0x20:0x400].split(b"\0", 1)[0].decode("ascii", "replace")
        self.dol_offset, self.fst_offset, self.fst_size = struct.unpack(">III", boot[0x420:0x42C])
        magic = struct.unpack(">I", boot[0x1C:0x20])[0]
        if magic != 0xC2339F3D:
            raise ValueError(f"{path}: not a GameCube image (magic {magic:#x})")

    def close(self) -> None:
        self._f.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def read(self, offset: int, size: int) -> bytes:
        self._f.seek(offset)
        data = self._f.read(size)
        if len(data) != size:
            raise EOFError(f"truncated read at {offset:#x}")
        return data

    def apploader_size(self) -> int:
        _date, _entry, size, trailer = struct.unpack(">16sIII", self.read(0x2440, 0x1C))
        return 0x20 + size + trailer

    def dol_size(self) -> int:
        """Size of the DOL: end of the furthest segment (7 text segments + 11 data segments)."""
        header = self.read(self.dol_offset, 0x100)
        offsets = struct.unpack(">18I", header[0x00:0x48])
        sizes = struct.unpack(">18I", header[0x90:0xD8])
        return max(o + s for o, s in zip(offsets, sizes) if s)

    def system_files(self) -> list[DiscFile]:
        """System files, extracted under `sys/` as Dolphin does."""
        return [
            DiscFile("sys/boot.bin", 0, 0x440),
            DiscFile("sys/bi2.bin", 0x440, 0x2000),
            DiscFile("sys/apploader.img", 0x2440, self.apploader_size()),
            DiscFile("sys/main.dol", self.dol_offset, self.dol_size()),
            DiscFile("sys/fst.bin", self.fst_offset, self.fst_size),
        ]

    def files(self) -> list[DiscFile]:
        """Files of the disc file system, under `files/`."""
        fst = self.read(self.fst_offset, self.fst_size)
        count = struct.unpack(">I", fst[8:12])[0]
        names = fst[count * 12:]

        def name_at(off: int) -> str:
            return names[off:names.index(b"\0", off)].decode("shift_jis", "replace")

        out: list[DiscFile] = []
        # Stack of (end index of the directory, directory path)
        stack: list[tuple[int, str]] = [(count, "files")]
        i = 1
        while i < count:
            while stack and i >= stack[-1][0]:
                stack.pop()
            word0, a, b = struct.unpack(">III", fst[i * 12:i * 12 + 12])
            is_dir = word0 >> 24
            name = name_at(word0 & 0xFFFFFF)
            parent = stack[-1][1]
            if is_dir:
                stack.append((b, f"{parent}/{name}"))
            else:
                out.append(DiscFile(f"{parent}/{name}", a, b))
            i += 1
        return out
