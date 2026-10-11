#!/usr/bin/env python3
"""Compare chefui captures with a design reference render.

    compare-ref.py OUT.png REF.png CAPTURE[:LABEL]... [--mask Y0-Y1:X0-X1]...

Each image is 1080x2246 (PNG or the PPM chefui_snapshot() writes). OUT.png
gets the reference and each capture side by side, then each capture's
difference from the reference (amplified 4x), at half size. Stdout gets,
per capture, the mean absolute difference and a band report: for every
48 px band of the reference that holds content, the vertical shift (within
+-24 px) that best lines the capture up with it and the residual
difference after that shift, then the horizontal shift (within +-24 px)
that best lines up the band's column profile (ink per column) once the
vertical shift is applied. A band that only matches far away, or not at
all, points at a layout difference rather than anti-aliasing.

--mask blanks a region in every image before comparing (for allowed
differences, for example the temperature the design shows and the app
does not). Needs Pillow; run with python3 -I.
"""
import sys

from PIL import Image, ImageChops, ImageDraw

BAND = 48
SEARCH = 24


def load(path):
    im = Image.open(path).convert("RGB")
    if im.size != (1080, 2246):
        sys.exit(f"{path}: {im.size}, expected 1080x2246")
    return im


def gray_rows(im, step=2):
    g = im.convert("L")
    w, h = g.size
    px = g.load()
    return [[px[x, y] for x in range(0, w, step)] for y in range(h)]


def col_profile(rows, y0, bg):
    return [sum(abs(rows[y][x] - bg) for y in range(y0, y0 + BAND)) for x in range(len(rows[0]))]


def best_dx(a, b):
    """Best horizontal shift; 0 unless another is clearly better (a band
    of full-width rules looks the same at every shift)."""
    n = len(a)
    cost = {dx: sum(abs(a[x] - b[x + dx]) for x in range(SEARCH, n - SEARCH))
            for dx in range(-SEARCH, SEARCH + 1)}
    dx = min(cost, key=lambda k: (cost[k], abs(k)))
    return 0 if cost[0] <= cost[dx] * 1.05 + 1 else dx


def band_report(ref, cap):
    rr, cr = gray_rows(ref), gray_rows(cap)
    rf, cf = gray_rows(ref, 1), gray_rows(cap, 1)
    bg = rf[2200][540]
    h = len(rr)
    out = []
    for y0 in range(0, h - BAND + 1, BAND):
        band = rr[y0:y0 + BAND]
        flat = [v for row in band for v in row]
        if max(flat) - min(flat) < 24:
            continue        # background only
        best = None
        for dy in range(-SEARCH, SEARCH + 1):
            if y0 + dy < 0 or y0 + dy + BAND > h:
                continue
            d = 0
            for i in range(BAND):
                a, b = band[i], cr[y0 + dy + i]
                d += sum(abs(p - q) for p, q in zip(a, b))
            d /= BAND * len(band[0])
            if best is None or d < best[1]:
                best = (dy, d)
        dx = best_dx(col_profile(rf, y0, bg), col_profile(cf, y0 + best[0], bg))
        out.append((y0, best[0], best[1], dx))
    return out


def apply_masks(im, masks, colour):
    if not masks:
        return im
    im = im.copy()
    d = ImageDraw.Draw(im)
    for (y0, y1, x0, x1) in masks:
        d.rectangle([x0, y0, x1, y1], fill=colour)
    return im


def main(argv):
    masks, args = [], []
    i = 0
    while i < len(argv):
        if argv[i] == "--mask":
            ys, xs = argv[i + 1].split(":")
            y0, y1 = map(int, ys.split("-"))
            x0, x1 = map(int, xs.split("-"))
            masks.append((y0, y1, x0, x1))
            i += 2
        else:
            args.append(argv[i])
            i += 1
    if len(args) < 3:
        sys.exit(__doc__)
    out, ref_path, caps = args[0], args[1], args[2:]
    ref = load(ref_path)
    bg = ref.getpixel((540, 2200))
    ref_m = apply_masks(ref, masks, bg)
    tiles, diffs = [ref], []
    print(f"reference {ref_path}")
    for c in caps:
        path, _, label = c.partition(":")
        im = load(path)
        tiles.append(im)
        im_m = apply_masks(im, masks, bg)
        diff = ImageChops.difference(ref_m, im_m)
        diffs.append(diff.point(lambda v: min(255, v * 4)))
        g = diff.convert("L")
        hist = g.histogram()
        mean = sum(v * n for v, n in enumerate(hist)) / (1080 * 2246)
        print(f"{label or path}: mean abs diff {mean:.2f}/255")
        for y0, dy, res, dx in band_report(ref_m, im_m):
            flag = "" if abs(dy) <= 3 and abs(dx) <= 3 and res < 12 else "   <--"
            print(f"  rows {y0:4d}-{y0 + BAND - 1:4d}: shift {dy:+3d} px, residual {res:5.1f}, "
                  f"x {dx:+3d} px{flag}")
    allt = tiles + diffs
    sheet = Image.new("RGB", (540 * len(allt) + 12 * (len(allt) - 1), 1123), (255, 0, 255))
    for k, im in enumerate(allt):
        sheet.paste(im.resize((540, 1123), Image.LANCZOS), (k * 552, 0))
    sheet.save(out)
    print(f"wrote {out}")


if __name__ == "__main__":
    main(sys.argv[1:])
