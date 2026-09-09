"""Design the color grid palettes off-device, then bake or push the winner.

The firmware shows one generated face four times in a 2x2 grid, each panel run
through its own palette (firmware/color_grid.c). Reflashing to compare two pinks is
miserable, so this reproduces that filter exactly - same integer luminance, same
histogram band edges, same softening, optionally the same RGB332 Floyd-Steinberg
the panel dithers with - and lets you look at dozens of candidates at once.

    # grab a face once, then iterate on it offline
    python viewer/view_serial.py --port /dev/ttyACM0 --seed 3
    python viewer/color_grid_tune.py --image out/seed_3.png --show
    python viewer/color_grid_tune.py --image out/seed_3.png --sheet --show
    python viewer/color_grid_tune.py --image out/seed_3.png --sweep soft --show
    python viewer/color_grid_tune.py --image out/seed_3.png --random 12 --show

    # bake the winner into the firmware, or try it on a running board
    python viewer/color_grid_tune.py --set palette2 --emit
    python viewer/color_grid_tune.py --set palette2 --push --port /dev/ttyACM0

Needs pillow + numpy (both in requirements.txt); --port needs pyserial.
"""
import argparse
import colorsys
import os
import random
import re
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
COLOR_GRID_C = os.path.join(ROOT, "firmware", "color_grid.c")

LEVELS = 4   # RF_COLOR_GRID_LEVELS
PANELS = 4   # RF_COLOR_GRID_PANELS
SRC_HW = 128 # RF_IMG_HW

# Candidate palette sets: four panels of LEVELS colours, darkest band first.
# The dark ink is shared within a set so eyes and hairline read the same in
# all four panels; the loud colours are the middle bands, where skin and hair
# land. Add your own here - a set is just 4 rows of LEVELS hex colours.
SETS = {
    "palette1": [  # the default baked into color_grid.c
        "1b0a1e e6007e ffd400 22c1c3",
        "06131f 0057ff 00e5a0 fff35c",
        "1f0606 ff2e00 ff9e00 ffe9c4",
        "0d0518 6a00f4 e040fb b8ff5c",
    ],
    "palette2": [  # one cream, one gold, a different accent per panel
        "14100c c8102e f5f0e1 e8c547",
        "0c1014 1d3f8f f5f0e1 e8c547",
        "0c1410 2e8b57 f5f0e1 e8c547",
        "140c14 6b2d5c f5f0e1 e8c547",
    ],
    "palette3": [  # loud flat colours on the same dark green
        "0a1a0a ff2d95 ffe14d 1f6b3a",
        "0a1a0a ff6a00 fff2b0 1f6b3a",
        "0a1a0a 00b3ff d6faff 1f6b3a",
        "0a1a0a b026ff ffd6ff 1f6b3a",
    ],
    "palette4": [  # maximum saturation, no shared ink - the loudest set
        "06131f 0057ff 00e5a0 fff35c",
        "1a0016 ff0080 00fff0 ffffff",
        "001a0e 00ff6a 003cff fffb00",
        "120014 ff4d00 ff00c8 8cffff",
    ],
    "palette5": [  # four duotones: the posterisation without the colour riot
        "0a0a0a 3d3d3d 8a8a8a f0f0f0",
        "0a1018 2a4a6a 6a9ac0 e8f4ff",
        "180a0a 6a2a2a c08a6a fff0e0",
        "0a180f 2a6a3d 8ac0a0 e8fff0",
    ],
}

DEFAULT_Q = (18, 46, 76)
DEFAULT_SOFT = 64


# ---- the filter, matching firmware/color_grid.c exactly ------------------------

def luma(rgb):
    """(77 R + 150 G + 29 B) >> 8, the integer luminance color_grid.c uses."""
    r, g, b = (rgb[..., i].astype(np.int32) for i in range(3))
    return (77 * r + 150 * g + 29 * b) >> 8


def band_edges(img, q):
    """Split luminance at the given population quantiles (percent, ascending).

    Taking the edges from the image's own histogram is what keeps a dark face
    and a bright one posterising in the same places. Mirrors rf_color_grid_levels().
    """
    lum = luma(img).ravel()
    hist = np.bincount(lum, minlength=256)
    n = lum.size
    edges = [0] * (LEVELS + 1)
    edges[LEVELS] = 256
    acc, v = 0, 0
    for k, qq in enumerate(q):
        target = n * qq // 100
        while v < 256 and acc + hist[v] <= target:
            acc += hist[v]
            v += 1
        edges[k + 1] = v
    return edges


def gradient_map(img, pal, edges, soft):
    """Luminance -> flat palette colour, softened toward the next band.

    soft=0 is a hard posterise; soft=255 is a smooth 4-point gradient map.
    Mirrors rf_color_grid_map().
    """
    lum = luma(img)
    lvl = np.zeros(lum.shape, np.int32)
    for k in range(1, LEVELS):
        lvl[lum >= edges[k]] = k
    lo = np.asarray(edges[:-1], np.int32)[lvl]
    hi = np.asarray(edges[1:], np.int32)[lvl]
    t = np.where(hi > lo, (lum - lo) * 256 // np.maximum(hi - lo, 1), 0)
    t = (t * soft) >> 8
    a = pal[lvl].astype(np.int32)
    b = pal[np.minimum(lvl + 1, LEVELS - 1)].astype(np.int32)
    return (a + (((b - a) * t[..., None]) >> 8)).astype(np.uint8)


def upscale(img, panel):
    """128 -> panel, nearest neighbour, the same index arithmetic as the C."""
    idx = (np.arange(panel) * SRC_HW) // panel
    return img[np.ix_(idx, idx)]


def color_grid(img, palettes, q=DEFAULT_Q, soft=DEFAULT_SOFT, panel=192):
    """One face -> the 2x2 recoloured grid the display shows."""
    edges = band_edges(img, q)
    cell = upscale(img, panel)
    out = np.zeros((panel * 2, panel * 2, 3), np.uint8)
    for p in range(PANELS):
        py, px = divmod(p, 2)
        out[py * panel:(py + 1) * panel, px * panel:(px + 1) * panel] = \
            gradient_map(cell, palettes[p], edges, soft)
    return out


def rgb332_dither(img, panel):
    """Floyd-Steinberg into RGB332, matching rf_vga_dither().

    The panel only has 256 colours, so a flat palette pink is not a colour it
    can hold - what you actually see is this. Slow (a Python pixel loop over
    the whole band), so it is opt-in for multi-cell output.
    """
    h, w = img.shape[:2]
    src = img.astype(np.int32)
    out = np.zeros((h, w, 3), np.uint8)
    err = [[[0] * 3 for _ in range(w + 2)] for _ in range(2)]
    for y in range(h):
        if y == panel:  # horizontal seam: don't rain one palette into the next
            err = [[[0] * 3 for _ in range(w + 2)] for _ in range(2)]
        cur = err[y & 1]
        err[(y & 1) ^ 1] = nxt = [[0] * 3 for _ in range(w + 2)]
        step = -1 if (y & 1) else 1
        x = w - 1 if (y & 1) else 0
        for _ in range(w):
            if x == (panel if step > 0 else panel - 1):  # vertical seam
                cur[x + 1] = [0] * 3
            for c in range(3):
                nb = 2 if c == 2 else 3
                v = src[y, x, c] + ((cur[x + 1][c] + 8) >> 4)
                v = 0 if v < 0 else 255 if v > 255 else v
                qn = v >> (8 - nb)
                rec = (((qn << 5) | (qn << 2) | (qn >> 1)) if nb == 3
                       else ((qn << 6) | (qn << 4) | (qn << 2) | qn))
                e = v - rec
                cur[x + step + 1][c] += 7 * e
                nxt[x - step + 1][c] += 3 * e
                nxt[x + 1][c] += 5 * e
                nxt[x + step + 1][c] += e
                out[y, x, c] = rec
            x += step
    return out


# ---- palettes in and out ---------------------------------------------------

def parse_set(rows):
    """4 rows of LEVELS hex colours -> (PANELS, LEVELS, 3) uint8."""
    pal = np.zeros((PANELS, LEVELS, 3), np.uint8)
    for p, row in enumerate(rows):
        cols = row.replace(",", " ").split()
        if len(cols) != LEVELS:
            raise SystemExit(f"palette {p}: need {LEVELS} colours, got {len(cols)}")
        for l, c in enumerate(cols):
            v = int(c.lstrip("#"), 16)
            pal[p, l] = (v >> 16, (v >> 8) & 0xFF, v & 0xFF)
    return pal


def fmt_set(pal):
    return [" ".join(f"{r:02x}{g:02x}{b:02x}" for r, g, b in row) for row in pal]


def random_set(rng):
    """A plausible color grid set: shared near-black ink, then three saturated
    hues spaced around the wheel at rising lightness."""
    rows = []
    ink_h = rng.random()
    for _ in range(PANELS):
        h0 = rng.random()
        cols = [tuple(int(255 * v) for v in
                      colorsys.hsv_to_rgb(ink_h, rng.uniform(0.3, 0.8),
                                          rng.uniform(0.05, 0.13)))]
        for k in range(1, LEVELS):
            h = (h0 + k * rng.uniform(0.18, 0.42)) % 1.0
            s = rng.uniform(0.55, 1.0) * (1.0 - 0.35 * (k == LEVELS - 1))
            v = 0.55 + 0.15 * k + rng.uniform(-0.08, 0.08)
            cols.append(tuple(int(255 * c) for c in
                              colorsys.hsv_to_rgb(h, min(s, 1.0), min(v, 1.0))))
        rows.append(" ".join(f"{r:02x}{g:02x}{b:02x}" for r, g, b in cols))
    return rows


def emit(pal, q, soft, label):
    """Rewrite the generated block in firmware/color_grid.c in place."""
    body = ["uint8_t rf_color_grid_pal[RF_COLOR_GRID_PANELS][RF_COLOR_GRID_LEVELS][3] = {"]
    for row in pal:
        cells = ", ".join("{0x%02x, 0x%02x, 0x%02x}" % tuple(int(v) for v in c)
                          for c in row)
        body.append(f"    {{{cells}}},")
    body.append("};")
    body.append("uint8_t rf_color_grid_q[RF_COLOR_GRID_LEVELS - 1] = {%s};"
                % ", ".join(str(v) for v in q))
    body.append(f"uint8_t rf_color_grid_soft = {soft};")

    head = ("/* ---- BEGIN GENERATED: viewer/color_grid_tune.py "
            f"--set {label} --emit ---- */\n")
    src = open(COLOR_GRID_C).read()
    pat = re.compile(r"/\* ---- BEGIN GENERATED.*?\*/\n.*?"
                     r"(/\* ---- END GENERATED)", re.S)
    if not pat.search(src):
        raise SystemExit(f"generated markers not found in {COLOR_GRID_C}")
    src = pat.sub(lambda m: head + "\n".join(body) + "\n" + m.group(1), src)
    open(COLOR_GRID_C, "w").write(src)
    print(f"wrote {COLOR_GRID_C} (rebuild the firmware to see it)")


def push(port, pal, q, soft):
    """Send the palette to a running board and make it re-render (L/W)."""
    import serial
    s = serial.Serial(port, 115200, timeout=5)
    for p, row in enumerate(pal):
        for l, c in enumerate(row):
            s.write(("L %d %d %02x%02x%02x\n" % (p, l, *(int(v) for v in c))).encode())
            print(s.readline().decode().strip())
    s.write(("W %d %s\n" % (soft, " ".join(str(v) for v in q))).encode())
    print(s.readline().decode().strip())


# ---- output ----------------------------------------------------------------

def load_source(args):
    from PIL import Image
    if args.port and not args.image:
        sys.path.insert(0, HERE)
        from view_serial import capture
        buf, w, h, ch, _, _ = capture(args.port, args.seed, args.steps,
                                      args.cls, args.cfg)
        im = Image.frombytes("L" if ch == 1 else "RGB", (w, h), buf)
    elif args.image.endswith((".rgb", ".gray")):
        buf = open(args.image, "rb").read()
        ch = 3 if args.image.endswith(".rgb") else 1
        im = Image.frombytes("L" if ch == 1 else "RGB", (SRC_HW, SRC_HW), buf)
    else:
        im = Image.open(args.image)
    return np.asarray(im.convert("RGB"), np.uint8)


def contact_sheet(cells, cols, pad=10):
    """cells: list of (label, HxWx3). Lays them out in a labelled grid."""
    from PIL import Image, ImageDraw
    ch_, cw = cells[0][1].shape[:2]
    rows = (len(cells) + cols - 1) // cols
    lab = 16
    sheet = Image.new("RGB", (cols * (cw + pad) + pad,
                              rows * (ch_ + lab + pad) + pad), (24, 24, 24))
    d = ImageDraw.Draw(sheet)
    for i, (label, arr) in enumerate(cells):
        r, c = divmod(i, cols)
        x, y = pad + c * (cw + pad), pad + r * (ch_ + lab + pad)
        sheet.paste(Image.fromarray(arr), (x, y))
        d.text((x + 2, y + ch_ + 2), label, fill=(220, 220, 220))
    return sheet


def main():
    ap = argparse.ArgumentParser(
        description="Preview and tune the firmware's color grid filter.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__.split("\n\n", 1)[1])
    src = ap.add_argument_group("source face")
    src.add_argument("--image", help="PNG, or a raw .rgb/.gray from the goldens")
    src.add_argument("--port", help="generate a fresh face from a board instead")
    src.add_argument("--seed", type=int, default=1)
    src.add_argument("--steps", type=int, default=4)
    src.add_argument("--class", "--cls", dest="cls", type=int, default=None)
    src.add_argument("--cfg", dest="cfg", type=int, default=None)

    knobs = ap.add_argument_group("filter")
    knobs.add_argument("--set", default="palette1", choices=sorted(SETS),
                       help="named palette set (default: palette1)")
    knobs.add_argument("--palette", action="append", metavar="HEX...",
                       help="one panel's colours, darkest first; repeat 4x to "
                            "replace --set entirely")
    knobs.add_argument("--soft", type=int, default=DEFAULT_SOFT,
                       help="0 flat posterise .. 255 smooth gradient map "
                            f"(default {DEFAULT_SOFT})")
    knobs.add_argument("--q", default=",".join(map(str, DEFAULT_Q)),
                       help="band split quantiles, percent, ascending "
                            f"(default {','.join(map(str, DEFAULT_Q))})")
    knobs.add_argument("--panel", type=int, default=192,
                       help="RF_PANEL: grid cell size, 192 or 216 (default 192)")

    mode = ap.add_argument_group("what to render")
    mode.add_argument("--sheet", action="store_true",
                      help="every named set side by side")
    mode.add_argument("--sweep", choices=("soft", "q"),
                      help="vary one knob across a row")
    mode.add_argument("--random", type=int, metavar="N",
                      help="N randomly generated candidate sets")
    mode.add_argument("--rng-seed", type=int, default=None)
    mode.add_argument("--dither", action="store_true",
                      help="simulate the panel's RGB332 Floyd-Steinberg "
                           "(what you actually see; slow)")
    mode.add_argument("--no-dither", dest="dither", action="store_false")
    ap.set_defaults(dither=None)

    out = ap.add_argument_group("output")
    out.add_argument("--out", default="out", help="output directory")
    out.add_argument("--show", action="store_true", help="open the result")
    out.add_argument("--emit", action="store_true",
                     help="write the palette into firmware/color_grid.c")
    out.add_argument("--push", action="store_true",
                     help="send the palette to the board on --port and redraw")
    args = ap.parse_args()

    q = tuple(int(v) for v in args.q.replace(",", " ").split())
    if len(q) != LEVELS - 1 or list(q) != sorted(q):
        raise SystemExit(f"--q needs {LEVELS - 1} ascending percentages")
    pal = parse_set(args.palette) if args.palette else parse_set(SETS[args.set])

    if args.emit:
        emit(pal, q, args.soft, "custom" if args.palette else args.set)
    if args.push:
        if not args.port:
            raise SystemExit("--push needs --port")
        push(args.port, pal, q, args.soft)
    if args.emit or args.push:
        # Both are actions in their own right. Only carry on to a preview if a
        # source face was named: falling through to --port here would spend 20s
        # generating a face nobody asked for, and show a different one from the
        # face --push just recoloured on the board.
        if not args.image:
            return
    elif not (args.image or args.port):
        raise SystemExit("need --image or --port (see --help)")

    img = load_source(args)
    multi = bool(args.sheet or args.sweep or args.random)
    dither = args.dither if args.dither is not None else not multi
    if dither and multi:
        print("dithering every cell; this takes a while", file=sys.stderr)

    def render(p, qq, soft):
        g = color_grid(img, p, qq, soft, args.panel)
        return rgb332_dither(g, args.panel) if dither else g

    from PIL import Image
    os.makedirs(args.out, exist_ok=True)
    if args.random:
        rng = random.Random(args.rng_seed)
        cells = []
        for i in range(args.random):
            rows = random_set(rng)
            cells.append((f"#{i}", render(parse_set(rows), q, args.soft)))
            print(f"#{i}:")
            for r in rows:
                print(f"    --palette '{r}'")
        result, name = contact_sheet(cells, min(4, len(cells))), "color_grid_random"
    elif args.sheet:
        cells = [(n, render(parse_set(SETS[n]), q, args.soft))
                 for n in sorted(SETS)]
        result, name = contact_sheet(cells, min(3, len(cells))), "color_grid_sets"
    elif args.sweep == "soft":
        vals = [0, 32, 64, 96, 128, 192, 255]
        cells = [(f"soft {v}", render(pal, q, v)) for v in vals]
        result, name = contact_sheet(cells, 4), "color_grid_sweep_soft"
    elif args.sweep == "q":
        vals = [(8, 30, 60), (12, 38, 68), (18, 46, 76), (24, 54, 82),
                (30, 62, 88), (10, 50, 90)]
        cells = [(",".join(map(str, v)), render(pal, v, args.soft)) for v in vals]
        result, name = contact_sheet(cells, 3), "color_grid_sweep_q"
    else:
        result = Image.fromarray(render(pal, q, args.soft))
        name = f"color_grid_{args.set}"
        print("band edges:", band_edges(img, q))

    path = os.path.join(args.out, name + ".png")
    result.save(path)
    print(f"saved to: '{path}'")
    if args.show:
        result.show()


if __name__ == "__main__":
    main()
