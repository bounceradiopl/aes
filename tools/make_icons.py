#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generates the icons of BounceAES Master ("BA") and Bounce Encoder ("BE").

    python3 tools/make_icons.py [--preview DIR]

Both icons share one layout: a near-black rounded square, a bold white monogram and the
pink dot of the bounce logo sitting on the baseline like a full stop. The tray variants show
the service state with a coloured dot in the bottom-right corner instead.

16, 20, 24 and 32 px are drawn by hand (pixel glyphs below) so they stay sharp in the tray;
larger sizes are rendered from a condensed bold system font (Helvetica Neue Condensed Bold on
macOS, Arial Narrow Bold or DejaVu Sans Condensed Bold elsewhere) with 8x supersampling. Fonts are used only here, nothing is
embedded. Needs Pillow.

Writes (paths relative to the repository root):
    tray/bounce.ico, tray/status-{ok,warn,down}.ico               BounceAES Master
    encoder_tray/encoder.ico, encoder_tray/status-{ok,warn,down}.ico  Bounce Encoder
    panel/favicon.png, panel/apple-touch-icon.png                 BounceAES Master panel
    encoder/panel/favicon.png, encoder/panel/apple-touch-icon.png Bounce Encoder panel
"""
import argparse
import io
import os
import struct
import sys

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

BG = (0x12, 0x12, 0x11)
INK = (0xFA, 0xF8, 0xF4)
BRAND = (0xF7, 0x28, 0x68)
STATE = {
    "ok": (0x8C, 0xC8, 0x84),
    "warn": (0xEB, 0xB2, 0x3F),
    "down": (0xE6, 0x45, 0x2E),
}

ICO_SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]
FONT_CANDIDATES = [
    ("/System/Library/Fonts/HelveticaNeue.ttc", 4),  # Helvetica Neue Condensed Bold
    ("/System/Library/Fonts/Supplemental/Arial Narrow Bold.ttf", 0),
    ("C:/Windows/Fonts/ARIALNB.TTF", 0),
    ("/usr/share/fonts/truetype/dejavu/DejaVuSansCondensed-Bold.ttf", 0),
]

# ---------------------------------------------------------------------------------------------
# Hand-drawn glyphs for the small sizes: '#' ink, '+' half-tone ink, '.' background.
GLYPHS = {
    16: {
        "B": ["####.",
              "##.##",
              "##.##",
              "####.",
              "##.##",
              "##.##",
              "##.##",
              "####."],
        "A": [".###.",
              "##.##",
              "##.##",
              "##.##",
              "#####",
              "##.##",
              "##.##",
              "##.##"],
        "E": ["#####",
              "##...",
              "##...",
              "####.",
              "##...",
              "##...",
              "##...",
              "#####"],
    },
    20: {
        "B": ["#####.",
              "##..##",
              "##..##",
              "##..##",
              "#####.",
              "##..##",
              "##..##",
              "##..##",
              "##..##",
              "#####."],
        "A": ["+####+",
              "##..##",
              "##..##",
              "##..##",
              "##..##",
              "######",
              "##..##",
              "##..##",
              "##..##",
              "##..##"],
        "E": ["######",
              "##....",
              "##....",
              "##....",
              "#####.",
              "##....",
              "##....",
              "##....",
              "##....",
              "######"],
    },
    24: {
        "B": ["######+",
              "#######",
              "##...##",
              "##...##",
              "##...##",
              "######+",
              "#######",
              "##...##",
              "##...##",
              "##...##",
              "#######",
              "######+"],
        "A": ["+#####+",
              "#######",
              "##...##",
              "##...##",
              "##...##",
              "##...##",
              "##...##",
              "#######",
              "#######",
              "##...##",
              "##...##",
              "##...##"],
        "E": ["######",
              "######",
              "##....",
              "##....",
              "##....",
              "#####.",
              "#####.",
              "##....",
              "##....",
              "##....",
              "######",
              "######"],
    },
    32: {
        "B": ["#######+.",
              "########.",
              "###..###.",
              "###..###.",
              "###..###.",
              "###..###.",
              "########.",
              "#########",
              "###...###",
              "###...###",
              "###...###",
              "###...###",
              "#########",
              "########+"],
        "A": ["+#######+",
              "#########",
              "###...###",
              "###...###",
              "###...###",
              "###...###",
              "###...###",
              "###...###",
              "###...###",
              "#########",
              "#########",
              "###...###",
              "###...###",
              "###...###"],
        "E": ["########",
              "########",
              "###.....",
              "###.....",
              "###.....",
              "###.....",
              "#######.",
              "#######.",
              "###.....",
              "###.....",
              "###.....",
              "###.....",
              "########",
              "########"],
    },
}

# Small-size layout: letter gap, full-stop dot size and gap, top row of the letters (base icon
# and tray), corner radius, tray status dot diameter (below the letters, bottom-right).
SMALL = {
    16: dict(gap=1, dot=2, dot_gap=1, top=4, tray_top=1, radius=2, status=6),
    20: dict(gap=1, dot=3, dot_gap=1, top=5, tray_top=1, radius=3, status=8),
    24: dict(gap=2, dot=3, dot_gap=1, top=6, tray_top=1, radius=3, status=10),
    32: dict(gap=2, dot=4, dot_gap=2, top=9, tray_top=2, radius=4, status=12),
}


def load_font(size):
    for path, index in FONT_CANDIDATES:
        if os.path.exists(path):
            return ImageFont.truetype(path, size, index=index)
    sys.exit("make_icons: no bold sans-serif font found (see FONT_CANDIDATES)")


def mix(a, b, t):
    return tuple(round(x + (y - x) * t) for x, y in zip(a, b))


def rounded_square(size, radius, k=8):
    """Background square with rounded corners, antialiased (supersampled k times)."""
    big = Image.new("L", (size * k, size * k), 0)
    if radius > 0:
        ImageDraw.Draw(big).rounded_rectangle((0, 0, size * k - 1, size * k - 1), radius * k, fill=255)
    else:
        big.paste(255, (0, 0, size * k, size * k))
    mask = big.resize((size, size), Image.BOX)
    img = Image.new("RGBA", (size, size), BG + (0,))
    img.putalpha(mask)
    return img


def disc(img, cx, cy, r, colour, k=16):
    """Paints an antialiased disc (centre and radius in pixels, may be fractional)."""
    size = img.size[0]
    big = Image.new("L", (size * k, size * k), 0)
    ImageDraw.Draw(big).ellipse(((cx - r) * k, (cy - r) * k, (cx + r) * k, (cy + r) * k), fill=255)
    cover = big.resize((size, size), Image.BOX)
    layer = Image.new("RGBA", (size, size), colour + (255,))
    layer.putalpha(cover)
    base_alpha = img.getchannel("A")
    out = Image.alpha_composite(img, layer)
    out.putalpha(base_alpha)  # keep the square's shape
    return out


def stamp(img, glyph, x0, y0):
    px = img.load()
    for y, row in enumerate(glyph):
        for x, ch in enumerate(row):
            if ch == ".":
                continue
            r, g, b, a = px[x0 + x, y0 + y]
            colour = INK if ch == "#" else mix(BG, INK, 0.45)
            px[x0 + x, y0 + y] = colour + (a,)


def small_icon(size, letters, state=None):
    p = SMALL[size]
    img = rounded_square(size, p["radius"])
    glyphs = [GLYPHS[size][c] for c in letters]
    widths = [len(g[0]) for g in glyphs]
    height = len(glyphs[0])
    letters_w = sum(widths) + p["gap"] * (len(glyphs) - 1)
    if state is None:
        group_w = letters_w + p["dot_gap"] + p["dot"]
        x = (size - group_w) // 2 + ((size - group_w) % 2)
        y = p["top"]
    else:
        x = (size - letters_w) // 2
        y = p["tray_top"]
    for g, w in zip(glyphs, widths):
        stamp(img, g, x, y)
        x += w + p["gap"]
    if state is None:
        d = p["dot"]
        x += p["dot_gap"] - p["gap"]
        img = disc(img, x + d / 2, y + height - d / 2, d / 2 + 0.25, BRAND)
    else:
        # below the letters, in the bottom-right corner: never touches a letter
        r = p["status"] / 2
        img = disc(img, size - 1 - r + 0.5, size - 1 - r + 0.5, r, STATE[state])
    return img


# Large-size layout, in fractions of the icon size.
CAP = 0.40          # cap height of the monogram (base icon)
GAP = 0.05          # between the letters
DOT = 0.12          # pink full stop
DOT_GAP = 0.04      # between the last letter and the full stop
TRAY_CAP = 0.40     # cap height in the tray variants
TRAY_TOP = 0.08     # top of the letters in the tray variants
TRAY_DOT = 0.40     # status dot diameter
TRAY_DOT_MARGIN = 0.03
RADIUS = 0.14


def large_icon(size, letters, state=None, radius_ratio=RADIUS, k=8):
    """Font-rendered icon for 40 px and up (and the panel PNGs)."""
    S = size * k
    cap = (CAP if state is None else TRAY_CAP) * S
    probe = load_font(100)
    cap_probe = -probe.getbbox("H", anchor="ls")[1]
    font = load_font(round(100 * cap / cap_probe))
    gap = GAP * S
    boxes = [font.getbbox(c, anchor="ls") for c in letters]
    widths = [b[2] - b[0] for b in boxes]
    letters_w = sum(widths) + gap * (len(letters) - 1)

    big = Image.new("RGBA", (S, S), BG + (255,))
    draw = ImageDraw.Draw(big)
    if state is None:
        dot, dot_gap = DOT * S, DOT_GAP * S
        x = (S - (letters_w + dot_gap + dot)) / 2
        baseline = (S + cap) / 2
    else:
        x = (S - letters_w) / 2
        baseline = TRAY_TOP * S + cap
    for c, b, w in zip(letters, boxes, widths):
        draw.text((x - b[0], baseline), c, font=font, fill=INK + (255,), anchor="ls")
        x += w + gap
    if state is None:
        x += dot_gap - gap
        draw.ellipse((x, baseline - dot, x + dot, baseline), fill=BRAND + (255,))
    else:
        d = TRAY_DOT * S
        e = S - TRAY_DOT_MARGIN * S
        draw.ellipse((e - d, e - d, e, e), fill=STATE[state] + (255,))
    mask = Image.new("L", (S, S), 0)
    if radius_ratio > 0:
        ImageDraw.Draw(mask).rounded_rectangle((0, 0, S - 1, S - 1), radius_ratio * S, fill=255)
    else:
        mask.paste(255, (0, 0, S, S))
    small = big.resize((size, size), Image.BOX)
    small.putalpha(mask.resize((size, size), Image.BOX))
    return small


def icon(size, letters, state=None):
    if size in SMALL:
        return small_icon(size, letters, state)
    return large_icon(size, letters, state)


# ---------------------------------------------------------------------------------------------
def write_ico(path, images):
    """ICO with 32-bit DIB frames below 256 px (read by every Windows API and NSIS) and a PNG
    frame at 256 px."""
    entries, blobs = [], []
    for img in images:
        w, h = img.size
        if w >= 256:
            buf = io.BytesIO()
            img.save(buf, "PNG", optimize=True)
            data = buf.getvalue()
        else:
            header = struct.pack("<IiiHHIIiiII", 40, w, h * 2, 1, 32, 0, 0, 0, 0, 0, 0)
            rows = []
            for y in range(h - 1, -1, -1):
                row = img.crop((0, y, w, y + 1)).tobytes("raw", "BGRA")
                rows.append(row)
            mask_stride = ((w + 31) // 32) * 4
            mask = bytearray()
            alpha = img.getchannel("A").load()
            for y in range(h - 1, -1, -1):
                bits = bytearray(mask_stride)
                for x in range(w):
                    if alpha[x, y] == 0:
                        bits[x // 8] |= 0x80 >> (x % 8)
                mask += bits
            data = header + b"".join(rows) + bytes(mask)
        entries.append((w, h, data))
        blobs.append(data)
    out = struct.pack("<HHH", 0, 1, len(entries))
    offset = 6 + 16 * len(entries)
    for w, h, data in entries:
        out += struct.pack("<BBBBHHII", w % 256, h % 256, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    out += b"".join(blobs)
    with open(path, "wb") as f:
        f.write(out)


def apps():
    return [
        ("BA", "tray/bounce.ico", "tray/status-{}.ico", "panel"),
        ("BE", "encoder_tray/encoder.ico", "encoder_tray/status-{}.ico", "encoder/panel"),
    ]


def build(root):
    for letters, app_ico, status_ico, panel in apps():
        write_ico(os.path.join(root, app_ico), [icon(s, letters) for s in ICO_SIZES])
        for state in STATE:
            write_ico(os.path.join(root, status_ico.format(state)), [icon(s, letters, state) for s in ICO_SIZES])
        icon(64, letters).save(os.path.join(root, panel, "favicon.png"), optimize=True)
        # iOS masks the touch icon itself: a full square without transparency
        touch = large_icon(180, letters, radius_ratio=0).convert("RGB")
        touch.save(os.path.join(root, panel, "apple-touch-icon.png"), optimize=True)


def preview(out):
    """Contact sheets: every size of every variant, 1:1 and enlarged, on dark and light."""
    os.makedirs(out, exist_ok=True)
    variants = [None] + list(STATE)
    for letters, *_ in apps():
        for state in variants:
            name = f"{letters}-{state or 'app'}"
            for s in ICO_SIZES:
                icon(s, letters, state).save(os.path.join(out, f"{name}-{s}.png"))
    for backdrop, colour in (("dark", (0x20, 0x20, 0x20)), ("light", (0xF3, 0xF3, 0xF3))):
        pad = 12
        rows = [(letters, state) for letters, *_ in apps() for state in variants]
        sizes = ICO_SIZES
        w = pad + sum(s + pad for s in sizes)
        h = pad + len(rows) * (256 + pad)
        sheet = Image.new("RGBA", (w, h), colour + (255,))
        y = pad
        for letters, state in rows:
            x = pad
            for s in sizes:
                im = icon(s, letters, state)
                sheet.alpha_composite(im, (x, y))
                x += s + pad
            y += 256 + pad
        sheet.save(os.path.join(out, f"sheet-{backdrop}.png"))
        # small sizes enlarged 8x (nearest neighbour) to judge the pixels
        zoom = 8
        small = [16, 20, 24, 32]
        w = pad + sum(s * zoom + pad for s in small)
        h = pad + len(rows) * (32 * zoom + pad)
        sheet = Image.new("RGBA", (w, h), colour + (255,))
        y = pad
        for letters, state in rows:
            x = pad
            for s in small:
                im = icon(s, letters, state).resize((s * zoom, s * zoom), Image.NEAREST)
                sheet.alpha_composite(im, (x, y))
                x += s * zoom + pad
            y += 32 * zoom + pad
        sheet.save(os.path.join(out, f"zoom-{backdrop}.png"))
        # the tray at 100 %, 125 %, 150 % and 200 %: both apps side by side, every state
        strip_sizes = [16, 20, 24, 32]
        w = pad + len(strip_sizes) * (8 * (32 + 6) + pad)
        h = pad * 2 + 32
        sheet = Image.new("RGBA", (w, h), colour + (255,))
        x = pad
        for s in strip_sizes:
            for letters, *_ in apps():
                for state in variants:
                    sheet.alpha_composite(icon(s, letters, state), (x, pad + (32 - s) // 2))
                    x += s + 6
            x += pad + 8 * (32 + 6) - 8 * (s + 6)
        sheet.save(os.path.join(out, f"tray-{backdrop}.png"))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--preview", metavar="DIR", help="also write preview PNGs to DIR")
    ap.add_argument("--no-write", action="store_true", help="only the previews, leave the repository alone")
    a = ap.parse_args()
    if not a.no_write:
        build(ROOT)
    if a.preview:
        preview(a.preview)


if __name__ == "__main__":
    main()
