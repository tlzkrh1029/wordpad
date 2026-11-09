#!/usr/bin/env python3
"""Generates res/app.ico (the RichPad application icon).

The artwork is drawn from scratch with Pillow: a sheet of paper with a folded corner,
a blue letter "A" with an orange underline and a few text lines.

Usage: python3 tools/make_icon.py [output.ico]
"""
import io
import struct
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

SIZES = [16, 20, 24, 32, 40, 48, 64, 256]
SS = 8  # supersampling factor

BLUE = (37, 99, 235, 255)
BLUE_DARK = (29, 78, 216, 255)
ORANGE = (245, 158, 11, 255)
PAPER = (255, 255, 255, 255)
PAPER_EDGE = (148, 163, 184, 255)
FOLD = (203, 213, 225, 255)
LINE = (148, 163, 184, 255)

FONT_CANDIDATES = [
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
    "C:/Windows/Fonts/segoeuib.ttf",
    "C:/Windows/Fonts/arialbd.ttf",
]


def load_font(px):
    for path in FONT_CANDIDATES:
        try:
            return ImageFont.truetype(path, px)
        except OSError:
            continue
    return ImageFont.load_default()


def draw_icon(size):
    n = size * SS
    img = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    def u(v):  # 0..100 design units -> pixels
        return v * n / 100.0

    edge = max(SS * 1.0, u(1.6))            # outline width (at least one final pixel)
    left, right, top, bottom = u(17), u(83), u(5), u(95)
    fold = u(22)
    radius = u(7)

    # Soft drop shadow for the larger sizes.
    if size >= 32:
        shadow = Image.new("RGBA", (n, n), (0, 0, 0, 0))
        sd = ImageDraw.Draw(shadow)
        off = u(2.2)
        sd.rounded_rectangle([left + off * 0.3, top + off, right + off * 0.3, bottom + off], radius, fill=(15, 23, 42, 46))
        sd.polygon([(right - fold + off, top - 1), (right + off + 1, top - 1), (right + off + 1, top + fold + off)],
                   fill=(0, 0, 0, 0))
        img = Image.alpha_composite(img, shadow)
        d = ImageDraw.Draw(img)

    # Sheet with a cut top-right corner.
    sheet = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    sd = ImageDraw.Draw(sheet)
    sd.rounded_rectangle([left, top, right, bottom], radius, fill=PAPER_EDGE)
    sd.rounded_rectangle([left + edge, top + edge, right - edge, bottom - edge], max(1, radius - edge), fill=PAPER)
    # Remove the corner, then draw the fold.
    sd.polygon([(right - fold, top - 1), (right + 1, top - 1), (right + 1, top + fold)], fill=(0, 0, 0, 0))
    sd.line([(right - fold, top + edge / 2), (right - edge / 2, top + fold)], fill=PAPER_EDGE, width=int(round(edge)))
    sd.polygon([(right - fold, top + edge / 2), (right - fold, top + fold), (right - edge / 2, top + fold)], fill=FOLD)
    sd.line([(right - fold, top + edge / 2), (right - fold, top + fold), (right - edge / 2, top + fold)],
            fill=PAPER_EDGE, width=int(round(edge)), joint="curve")
    img = Image.alpha_composite(img, sheet)
    d = ImageDraw.Draw(img)

    # Letter "A" with an underline.
    if size >= 24:
        a_height = u(34)
        font = load_font(int(a_height * 1.32))
        ax, ay = u(27), u(16)
        box = d.textbbox((0, 0), "A", font=font)
        d.text((ax - box[0], ay - box[1]), "A", font=font, fill=BLUE)
        a_w = box[2] - box[0]
        a_h = box[3] - box[1]
        bar_top = ay + a_h + u(4)
        d.rounded_rectangle([ax, bar_top, ax + a_w, bar_top + u(5.5)], u(2), fill=ORANGE)
        text_top = bar_top + u(13)
        lw = max(SS * 1.0, u(4.2))
        # Full-width text lines below.
        count = 3 if size >= 40 else 2
        for i in range(count):
            yy = text_top + i * u(10.5)
            if yy + lw > bottom - u(7):
                break
            end = right - u(10) if i < count - 1 else right - u(26)
            d.rounded_rectangle([left + u(10), yy, end, yy + lw], lw / 2, fill=LINE)
    else:
        # Tiny sizes: a bold blue "A", an orange underline and one text line.
        font = load_font(int(u(46)))
        ax, ay = u(27), u(22)
        box = d.textbbox((0, 0), "A", font=font)
        d.text((ax - box[0], ay - box[1]), "A", font=font, fill=BLUE_DARK)
        a_w = box[2] - box[0]
        a_h = box[3] - box[1]
        bar = max(SS * 1.0, u(6))
        y1 = ay + a_h + u(6)
        d.rectangle([ax, y1, ax + a_w, y1 + bar], fill=ORANGE)
        y2 = y1 + bar + u(7)
        if y2 + bar <= bottom - u(6):
            d.rectangle([ax, y2, right - u(12), y2 + bar], fill=LINE)

    return img.resize((size, size), Image.LANCZOS)


def bmp_entry(img):
    """32-bit BGRA DIB with an (empty) AND mask, as stored inside .ico files."""
    w, h = img.size
    pixels = img.tobytes("raw", "BGRA")
    rows = [pixels[i * w * 4:(i + 1) * w * 4] for i in range(h)]
    xor = b"".join(reversed(rows))
    mask_row = ((w + 31) // 32) * 4
    and_mask = b"\x00" * (mask_row * h)
    header = struct.pack("<IiiHHIIiiII", 40, w, h * 2, 1, 32, 0, len(xor) + len(and_mask), 0, 0, 0, 0)
    return header + xor + and_mask


def png_entry(img):
    buf = io.BytesIO()
    img.save(buf, format="PNG", optimize=True)
    return buf.getvalue()


def main():
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parent.parent / "res" / "app.ico"
    images = [(s, draw_icon(s)) for s in SIZES]
    blobs = [(s, png_entry(im) if s >= 256 else bmp_entry(im)) for s, im in images]
    data = struct.pack("<HHH", 0, 1, len(blobs))
    offset = 6 + 16 * len(blobs)
    for s, blob in blobs:
        data += struct.pack("<BBBBHHII", s % 256, s % 256, 0, 0, 1, 32, len(blob), offset)
        offset += len(blob)
    for _, blob in blobs:
        data += blob
    out.write_bytes(data)
    print(f"wrote {out} ({len(data)} bytes)")
    if len(sys.argv) > 2:  # optional preview sheet
        sheet = Image.new("RGBA", (sum(s for s, _ in images) + 20 * (len(images) + 1), 300), (240, 242, 245, 255))
        x = 20
        for s, im in images:
            sheet.alpha_composite(im, (x, (300 - s) // 2))
            x += s + 20
        sheet.save(sys.argv[2])


if __name__ == "__main__":
    main()
