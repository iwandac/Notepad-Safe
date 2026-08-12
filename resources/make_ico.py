# -*- coding: utf-8 -*-
"""Generate a valid multi-size Windows .ico (6 sizes: 16/32/48/64/128/256)
from notepad_safe.png using Pillow.
"""
from PIL import Image, BmpImagePlugin
from pathlib import Path
import io, struct, sys

SIZES = (16, 32, 48, 64, 128, 256)

def make_bmp_bytes(img: Image.Image) -> bytes:
    """Convert RGBA image to BMP-without-fileheader bytes + AND mask bytes,
    which is exactly what a standard ICO image entry expects.
    Height in the BMP info header is doubled (XOR + AND)."""
    w, h = img.size
    img = img.convert("RGBA")
    # Build XOR DIB bits (bottom-up, 32bpp BGRA padded to 4 bytes per row — 32bpp already aligned)
    pixels = img.tobytes("raw", "BGRA", 0, -1)  # bottom-up BGRA
    # AND mask: 1bpp, rows padded to 32-bit boundary
    stride = ((w + 31) // 32) * 4
    and_bytes = bytearray()
    for y in range(h):
        row_mask = 0
        bit = 0x80000000 >> (w - 1) if w <= 32 else None
        # Compute AND mask bits: 1 if pixel alpha == 0 (transparent), 0 otherwise
        for x in range(w):
            a = img.getpixel((x, y))[3]
            if a < 128:
                # Set 1 in AND mask at this bit position
                byte_idx = x // 8
                bit_idx = 7 - (x % 8)
                # we'll build row bytes via bytearray directly
        # Build proper via simpler pixel iteration
        row_bytes = bytearray(stride)
        for x in range(w):
            if img.getpixel((x, y))[3] < 128:
                row_bytes[x // 8] |= (1 << (7 - (x % 8)))
        and_bytes += bytes(row_bytes)

    xor_size = w * h * 4
    and_size = stride * h
    img_size = xor_size + and_size

    # BITMAPINFOHEADER (40 bytes), biHeight = 2*h for ICO format
    bih = struct.pack("<IiiHHIIiiII",
                      40,         # biSize
                      w,          # biWidth
                      2 * h,      # biHeight (XOR + AND)
                      1,          # biPlanes
                      32,         # biBitCount
                      0,          # biCompression (BI_RGB)
                      img_size,   # biSizeImage
                      0, 0, 0, 0)
    return bytes(bih) + pixels + bytes(and_bytes)

def main():
    root = Path(__file__).resolve().parent
    png_path = root / "notepad_safe.png"
    ico_path = root / "app.ico"
    src = Image.open(png_path).convert("RGBA")

    images_data = []
    for sz in SIZES:
        resized = src.resize((sz, sz), Image.LANCZOS)
        # Also keep a 256 entry PNG version for Vista+ (optional)
        images_data.append((sz, make_bmp_bytes(resized)))

    # Build ICO file manually to guarantee 6 entries
    count = len(images_data)
    header = struct.pack("<HHH", 0, 1, count)  # reserved, type=ICO, count
    dir_size = 16 * count
    entries = bytearray()
    data_blobs = bytearray()
    data_offset = len(header) + dir_size
    for sz, blob in images_data:
        w = 0 if sz >= 256 else sz
        h = 0 if sz >= 256 else sz
        entries += struct.pack("<BBBBHHII",
                               w & 0xFF, h & 0xFF,  # width, height
                               0, 0,                # color count, reserved
                               1, 32,               # planes, bitcount
                               len(blob),
                               data_offset)
        data_offset += len(blob)
        data_blobs += blob
    with open(ico_path, "wb") as f:
        f.write(header)
        f.write(bytes(entries))
        f.write(bytes(data_blobs))
    total = ico_path.stat().st_size
    print(f"OK wrote {ico_path} = {total} bytes, {count} entries: {list(SIZES)}")

if __name__ == "__main__":
    main()
