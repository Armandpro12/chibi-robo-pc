"""Renders a simple flat icon, described by geometric shapes in a TOML file, into a multi-size Windows .ico file.

Usage:
  py -3 tools/make_icon.py <design.toml> <out.ico>           write the icon
  py -3 tools/make_icon.py <design.toml> <out.ico> --check   exit 1 if <out.ico> differs from a fresh rendering
                                                             (same bytes, or same decoded pixels, see below)
  py -3 tools/make_icon.py <design.toml> <out.ico> --png DIR also write one PNG per size into DIR (to look at it)

Generic: the design (shapes and colours) comes from the game folder, e.g. games/<game>/icon.toml. The renderer is
pure Python (no third-party module), so the output is deterministic: the same design always gives the same bytes
with the same zlib build. The 256 x 256 PNG is compressed by zlib, whose output may differ between builds (same
pixels, other bytes): --check therefore accepts a file whose bytes differ if every image decodes (PNG inflated and
unfiltered, DIB read with its AND mask) to the same pixels, and the same sizes, as a fresh rendering.

Design file:
  sizes = [16, 24, 32, 48, 64, 128, 256]     # square sizes, in pixels; 256 is stored as PNG, the others as BMP
  [[shape]]                                 # drawn in order, each one over the previous ones
  kind = "rect"                             # "rect" (rounded rectangle) or "circle"
  box = [x0, y0, x1, y1]                    # rect: corners, in units of the icon size (0 = left/top, 1 = right/bottom)
  radius = 0.1                              # rect: corner radius (same units); circle: radius
  center = [x, y]                           # circle: centre (same units)
  color = "#RRGGBB" or "#RRGGBBAA"

Anti-aliasing: each pixel takes the coverage given by the signed distance from its centre to the shape's edge
(clamped to one pixel), which is exact for these shapes up to the corners' curvature.

ICO layout (Microsoft, "Icons" in the Win32 documentation, and the ICONDIR / ICONDIRENTRY structures it describes):
an ICONDIR header, one 16-byte entry per image, then each image: a BITMAPINFOHEADER with double height, the 32-bit
BGRA pixels bottom-up and the 1-bit AND mask; or, for 256 x 256, a whole PNG file (supported since Windows Vista).
"""

from __future__ import annotations

import argparse
import math
import struct
import sys
import tomllib
import zlib
from pathlib import Path


def parse_color(text: str) -> tuple[float, float, float, float]:
    t = text.lstrip("#")
    if len(t) not in (6, 8):
        raise SystemExit(f"bad colour {text!r}: expected #RRGGBB or #RRGGBBAA")
    v = [int(t[i:i + 2], 16) / 255.0 for i in range(0, len(t), 2)]
    return (v[0], v[1], v[2], v[3] if len(v) == 4 else 1.0)


def sd_round_rect(px: float, py: float, box: list[float], r: float) -> float:
    """Signed distance from (px, py) to a rounded rectangle (negative inside)."""
    cx, cy = (box[0] + box[2]) / 2, (box[1] + box[3]) / 2
    hx, hy = (box[2] - box[0]) / 2 - r, (box[3] - box[1]) / 2 - r
    qx, qy = abs(px - cx) - hx, abs(py - cy) - hy
    outside = math.hypot(max(qx, 0.0), max(qy, 0.0))
    inside = min(max(qx, qy), 0.0)
    return outside + inside - r


def sd_circle(px: float, py: float, c: list[float], r: float) -> float:
    return math.hypot(px - c[0], py - c[1]) - r


def render(shapes: list[dict], size: int) -> list[tuple[int, int, int, int]]:
    """RGBA pixels (straight alpha), top row first."""
    prepared = []
    for s in shapes:
        col = parse_color(s["color"])
        if s["kind"] == "rect":
            box = s["box"]
            r = float(s.get("radius", 0.0))
            prepared.append((col, lambda x, y, box=box, r=r: sd_round_rect(x, y, box, r),
                             (box[0], box[1], box[2], box[3])))
        elif s["kind"] == "circle":
            c, r = s["center"], float(s["radius"])
            prepared.append((col, lambda x, y, c=c, r=r: sd_circle(x, y, c, r),
                             (c[0] - r, c[1] - r, c[0] + r, c[1] + r)))
        else:
            raise SystemExit(f"unknown shape kind {s['kind']!r}")
    out = []
    px_size = 1.0 / size
    for j in range(size):
        y = (j + 0.5) * px_size
        for i in range(size):
            x = (i + 0.5) * px_size
            r = g = b = a = 0.0  # premultiplied while compositing
            for col, sd, bb in prepared:
                if x < bb[0] - px_size or x > bb[2] + px_size or y < bb[1] - px_size or y > bb[3] + px_size:
                    continue
                cov = min(max(0.5 - sd(x, y) / px_size, 0.0), 1.0) * col[3]
                if cov <= 0.0:
                    continue
                r = col[0] * cov + r * (1 - cov)
                g = col[1] * cov + g * (1 - cov)
                b = col[2] * cov + b * (1 - cov)
                a = cov + a * (1 - cov)
            if a > 0:
                out.append((round(r / a * 255), round(g / a * 255), round(b / a * 255), round(a * 255)))
            else:
                out.append((0, 0, 0, 0))
    return out


def png_bytes(pixels: list[tuple[int, int, int, int]], size: int) -> bytes:
    """8-bit RGBA PNG (PNG specification, W3C, 2003): filter 0 on every row, zlib level 9."""
    raw = bytearray()
    for j in range(size):
        raw.append(0)
        for p in pixels[j * size:(j + 1) * size]:
            raw += bytes(p)

    def chunk(tag: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))


def bmp_bytes(pixels: list[tuple[int, int, int, int]], size: int) -> bytes:
    """Icon image as a DIB: BITMAPINFOHEADER (height doubled: colour + mask), BGRA bottom-up, AND mask (1 = transparent)."""
    mask_stride = ((size + 31) // 32) * 4
    xor = bytearray()
    mask = bytearray()
    for j in range(size - 1, -1, -1):
        row = pixels[j * size:(j + 1) * size]
        for (r, g, b, a) in row:
            xor += bytes((b, g, r, a))
        bits = bytearray(mask_stride)
        for i, p in enumerate(row):
            if p[3] == 0:
                bits[i // 8] |= 0x80 >> (i % 8)
        mask += bits
    header = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0, len(xor) + len(mask), 0, 0, 0, 0)
    return header + bytes(xor) + bytes(mask)


def build_ico(design: dict) -> tuple[bytes, dict[int, list]]:
    sizes = [int(s) for s in design.get("sizes", [16, 24, 32, 48, 64, 128, 256])]
    if not sizes or any(s < 1 or s > 256 for s in sizes) or len(set(sizes)) != len(sizes):
        raise SystemExit(f"bad sizes {sizes}: distinct values from 1 to 256")
    shapes = design.get("shape", [])
    if not shapes:
        raise SystemExit("the design has no [[shape]]")
    images, rendered = [], {}
    for s in sizes:
        px = render(shapes, s)
        rendered[s] = px
        images.append(png_bytes(px, s) if s == 256 else bmp_bytes(px, s))
    head = struct.pack("<HHH", 0, 1, len(sizes))
    offset = 6 + 16 * len(sizes)
    entries = b""
    for s, data in zip(sizes, images):
        dim = 0 if s == 256 else s
        entries += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    return head + entries + b"".join(images), rendered


def decode_png(data: bytes) -> tuple[int, int, list[tuple[int, int, int, int]]]:
    """Pixels of an 8-bit RGBA, non-interlaced PNG (PNG specification, W3C, 2003: filter types 0 to 4)."""
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos, idat, ihdr = 8, bytearray(), None
    while pos + 8 <= len(data):
        length, tag = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        if len(body) != length:
            raise ValueError("truncated PNG chunk")
        if tag == b"IHDR":
            ihdr = struct.unpack(">IIBBBBB", body)
        elif tag == b"IDAT":
            idat += body
        elif tag == b"IEND":
            break
        pos += 12 + length
    if ihdr is None or ihdr[2:] != (8, 6, 0, 0, 0):
        raise ValueError(f"unsupported PNG header {ihdr}")
    w, h = ihdr[0], ihdr[1]
    raw = zlib.decompress(bytes(idat))
    stride = w * 4
    if len(raw) != h * (stride + 1):
        raise ValueError("PNG data has the wrong length")
    prev = bytearray(stride)
    pixels = []
    for j in range(h):
        ft = raw[j * (stride + 1)]
        line = bytearray(raw[j * (stride + 1) + 1:(j + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - 4] if i >= 4 else 0
            b = prev[i]
            c = prev[i - 4] if i >= 4 else 0
            if ft == 1:
                line[i] = (line[i] + a) & 255
            elif ft == 2:
                line[i] = (line[i] + b) & 255
            elif ft == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif ft == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pred = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[i] = (line[i] + pred) & 255
            elif ft != 0:
                raise ValueError(f"bad PNG filter type {ft}")
        pixels += [tuple(line[i:i + 4]) for i in range(0, stride, 4)]
        prev = line
    return w, h, pixels


def decode_dib(data: bytes) -> tuple[int, list[tuple[int, int, int, int]]]:
    """Pixels of an icon DIB written by bmp_bytes (32-bit BGRA bottom-up + 1-bit AND mask, height doubled). The AND
    mask must agree with the alpha channel (set where alpha is 0), as the encoder writes it."""
    hdr, w, h2, planes, bpp, comp = struct.unpack("<IiiHHI", data[:20])
    size = w
    if hdr != 40 or h2 != 2 * w or planes != 1 or bpp != 32 or comp != 0:
        raise ValueError(f"unsupported DIB header {(hdr, w, h2, planes, bpp, comp)}")
    xor_len = size * size * 4
    mask_stride = ((size + 31) // 32) * 4
    if len(data) != 40 + xor_len + mask_stride * size:
        raise ValueError("DIB data has the wrong length")
    rows = []
    for j in range(size):  # stored bottom-up
        row = data[40 + j * size * 4:40 + (j + 1) * size * 4]
        rows.append([(row[i + 2], row[i + 1], row[i], row[i + 3]) for i in range(0, size * 4, 4)])
    rows.reverse()
    mask = data[40 + xor_len:]
    for j in range(size):
        mrow = mask[(size - 1 - j) * mask_stride:(size - j) * mask_stride]
        for i in range(size):
            if bool(mrow[i // 8] & (0x80 >> (i % 8))) != (rows[j][i][3] == 0):
                raise ValueError(f"AND mask disagrees with alpha at ({i}, {j})")
    return size, [p for row in rows for p in row]


def decode_ico(data: bytes) -> dict[int, list[tuple[int, int, int, int]]]:
    """Per size, the RGBA pixels (top row first) of every image of an .ico file, in file order."""
    reserved, kind, count = struct.unpack("<HHH", data[:6])
    if reserved != 0 or kind != 1:
        raise ValueError("not an .ico file")
    out = {}
    for n in range(count):
        dim, _h, _c, _r, _planes, _bpp, length, offset = struct.unpack("<BBBBHHII", data[6 + 16 * n:22 + 16 * n])
        img = data[offset:offset + length]
        if len(img) != length:
            raise ValueError("truncated image")
        if img[:8] == b"\x89PNG\r\n\x1a\n":
            size, h, px = decode_png(img)
            if size != h:
                raise ValueError("non-square PNG")
        else:
            size, px = decode_dib(img)
        if size in out or (dim or 256) != size:
            raise ValueError(f"image {n}: size {size} repeated or different from its directory entry ({dim})")
        out[size] = px
    return out


def compare_ico(existing: bytes, rendered: dict[int, list]) -> str | None:
    """None if every image of `existing` has the pixels of the fresh rendering (and the same sizes, same order);
    else a message saying what differs."""
    try:
        have = decode_ico(existing)
    except (ValueError, struct.error, zlib.error) as e:
        return f"cannot decode the existing file: {e}"
    if list(have) != list(rendered):
        return f"sizes {list(have)} instead of {list(rendered)}"
    for size, px in rendered.items():
        if have[size] != px:
            n = next(i for i, (a, b) in enumerate(zip(have[size], px)) if a != b)
            return (f"size {size}: pixel ({n % size}, {n // size}) is {have[size][n]}, a fresh rendering gives "
                    f"{px[n]}")
    return None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("design")
    ap.add_argument("out")
    ap.add_argument("--check", action="store_true", help="compare with the existing file instead of writing it")
    ap.add_argument("--png", metavar="DIR", help="also write one PNG per size into DIR")
    args = ap.parse_args()
    design = tomllib.loads(Path(args.design).read_text(encoding="utf-8"))
    data, rendered = build_ico(design)
    out = Path(args.out)
    if args.png:
        d = Path(args.png)
        d.mkdir(parents=True, exist_ok=True)
        for s, px in rendered.items():
            (d / f"{out.stem}-{s}.png").write_bytes(png_bytes(px, s))
    if args.check:
        if not out.is_file():
            print(f"{out}: DIFFERS from the design (missing; run without --check)")
            return 1
        existing = out.read_bytes()
        if existing == data:
            print(f"{out}: up to date")
            return 0
        problem = compare_ico(existing, rendered)
        if problem is None:
            print(f"{out}: up to date (same pixels; the bytes differ, probably another zlib build)")
            return 0
        print(f"{out}: DIFFERS from the design (run without --check): {problem}")
        return 1
    out.write_bytes(data)
    print(f"{out}: {len(data)} bytes, sizes {', '.join(str(s) for s in rendered)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
