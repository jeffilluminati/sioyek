#!/usr/bin/env python3
"""Generate PDFs that exercise the rasterizer's and the image scaler's corner cases, to check that changes to
them (e.g. mupdf-patches/) leave the rendered output identical. Compare two builds with compare_output.sh.

  shapes_40.pdf   even-odd and non-zero fills of self-intersecting paths, holes, clip paths (both rules),
                  transparency, soft masks, dashes, caps and joins, hairlines, curves, shapes crossing the
                  page edges, stroked and clipping text, tiny shapes
  images_24.pdf   gray, RGB and CMYK images (some with soft masks) drawn flipped, mirrored, up and down
                  scaled by various factors, 1 pixel wide or high, with odd sizes, partly off the page

usage: gen_render_tests.py [output directory]
"""
import math
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_corpus import PdfWriter, build_doc

OUT = sys.argv[1] if len(sys.argv) > 1 else "."


def star(rnd, cx, cy, r, n):
    pts = []
    for k in range(n):
        a = 2 * math.pi * k * (n // 2) / n + rnd.random() * 0.2
        pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    s = "%.2f %.2f m " % pts[0] + " ".join("%.2f %.2f l" % p for p in pts[1:]) + " h\n"
    return s

def blob(rnd, cx, cy, r):
    s = "%.2f %.2f m\n" % (cx + r, cy)
    for k in range(1, 9):
        a0 = 2 * math.pi * (k - 1) / 8
        a1 = 2 * math.pi * k / 8
        rr = r * (0.6 + rnd.random() * 0.8)
        s += "%.2f %.2f %.2f %.2f %.2f %.2f c\n" % (cx + rr * math.cos(a0 + 0.3), cy + rr * math.sin(a0 + 0.3),
                                                     cx + rr * math.cos(a1 - 0.3), cy + rr * math.sin(a1 - 0.3),
                                                     cx + r * math.cos(a1), cy + r * math.sin(a1))
    return s + "h\n"

def shapes_page(w, i):
    rnd = random.Random(1000 + i)
    o = []
    rgb = lambda: "%.3f %.3f %.3f" % (rnd.random(), rnd.random(), rnd.random())
    kind = i % 10
    if kind == 0:  # self-intersecting stars, both fill rules
        for _ in range(40):
            o.append(rgb() + " rg\n" + star(rnd, rnd.uniform(-50, 660), rnd.uniform(-50, 840), rnd.uniform(3, 200), rnd.choice([5, 7, 9, 11, 17])) + rnd.choice(["f", "f*", "B", "B*"]) + "\n")
    elif kind == 1:  # rings: outer and reversed inner subpaths, non-zero and even-odd
        for _ in range(60):
            cx, cy, r = rnd.uniform(0, 612), rnd.uniform(0, 792), rnd.uniform(2, 120)
            o.append(rgb() + " rg\n" + blob(rnd, cx, cy, r) + blob(rnd, cx, cy, r * 0.5) + rnd.choice(["f", "f*"]) + "\n")
    elif kind == 2:  # clip paths then fills
        for _ in range(12):
            o.append("q\n" + star(rnd, rnd.uniform(50, 560), rnd.uniform(50, 740), rnd.uniform(40, 300), rnd.choice([5, 7, 13])) + rnd.choice(["W n", "W* n"]) + "\n")
            o.append(blob(rnd, rnd.uniform(50, 560), rnd.uniform(50, 740), rnd.uniform(40, 200)) + "W n\n")
            for _ in range(30):
                o.append(rgb() + " rg %.1f %.1f %.1f %.1f re f\n" % (rnd.uniform(0, 612), rnd.uniform(0, 792), rnd.uniform(1, 150), rnd.uniform(1, 150)))
            o.append(rgb() + " RG 3 w " + blob(rnd, rnd.uniform(50, 560), rnd.uniform(50, 740), rnd.uniform(40, 200)) + "S\nQ\n")
    elif kind == 3:  # transparency and soft masks
        for _ in range(30):
            o.append("q /GS1 gs " + rgb() + " rg " + rgb() + " RG 4 w " + blob(rnd, rnd.uniform(0, 612), rnd.uniform(0, 792), rnd.uniform(10, 150)) + "B\nQ\n")
        o.append("q /GS2 gs 0.1 0.4 0.8 rg 0 0 612 792 re f Q\n")
        for _ in range(10):
            o.append("q /GS2 gs " + rgb() + " rg " + star(rnd, rnd.uniform(0, 612), rnd.uniform(0, 792), rnd.uniform(20, 200), 7) + "f*\nQ\n")
    elif kind == 4:  # dashes, caps, joins, widths incl. hairlines
        for _ in range(60):
            o.append("%s RG %.2f w %d J %d j [%s] %.1f d\n" % (rgb(), rnd.choice([0, 0.01, 0.1, 0.3, 1, 2.5, 7, 15]), rnd.randrange(3), rnd.randrange(3),
                     " ".join("%.1f" % rnd.uniform(0.5, 12) for _ in range(rnd.choice([0, 1, 2, 4]))), rnd.uniform(0, 5)))
            x, y = rnd.uniform(0, 612), rnd.uniform(0, 792)
            s = "%.2f %.2f m" % (x, y)
            for _ in range(rnd.randrange(1, 12)):
                x += rnd.uniform(-80, 80); y += rnd.uniform(-80, 80)
                s += " %.2f %.2f l" % (x, y)
            o.append(s + (" h S\n" if rnd.random() < 0.3 else " S\n"))
    elif kind == 5:  # dense plot like the vector corpus, with curves and a few transforms
        for series in range(6):
            o.append("q %.3f %.3f %.3f %.3f %.1f %.1f cm %s RG %.2f w\n" % (1, rnd.uniform(-0.2, 0.2), rnd.uniform(-0.2, 0.2), 1, rnd.uniform(-20, 20), rnd.uniform(-20, 20), rgb(), rnd.uniform(0.2, 2)))
            x, y = 30.0, rnd.uniform(100, 700)
            s = "%.2f %.2f m\n" % (x, y)
            for _ in range(300):
                x2, y2 = x + 1.8, y + rnd.uniform(-10, 10)
                s += "%.2f %.2f %.2f %.2f %.2f %.2f c\n" % (x + 0.6, y + rnd.uniform(-5, 5), x + 1.2, y2 + rnd.uniform(-5, 5), x2, y2)
                x, y = x2, y2
            o.append(s + "S Q\n")
    elif kind == 6:  # stroked and clipping text, tiny shapes
        o.append("BT /F1 36 Tf 1 Tr 0.5 w 0.8 0.1 0.1 RG 40 700 Td (Stroked text Wg) Tj ET\n")
        o.append("BT /F1 30 Tf 2 Tr 0.3 w 0.1 0.1 0.8 RG 0.9 0.9 0.2 rg 40 640 Td (Fill and stroke) Tj ET\n")
        o.append("q BT /F1 60 Tf 7 Tr 40 520 Td (CLIPPED) Tj ET\n")
        for k in range(40):
            o.append(rgb() + " rg %d 500 8 90 re f\n" % (40 + k * 12))
        o.append("Q\n")
        for _ in range(400):
            o.append(rgb() + " rg %.2f %.2f %.2f %.2f re f\n" % (rnd.uniform(0, 612), rnd.uniform(0, 480), rnd.uniform(0.01, 3), rnd.uniform(0.01, 3)))
    elif kind == 7:  # shapes far outside and crossing the page edges, huge coordinates
        for _ in range(40):
            o.append(rgb() + " rg " + star(rnd, rnd.choice([-300, 0, 612, 900]), rnd.uniform(-400, 1200), rnd.uniform(100, 900), rnd.choice([5, 9])) + rnd.choice(["f", "f*"]) + "\n")
        o.append("0 0 0 RG 2 w -100000 -100000 m 100000 100000 l S\n")
        o.append("0.2 0.6 0.2 rg -50000 300 m 50000 310 l 50000 320 l -50000 330 l h f\n")
    elif kind == 8:  # rectangles and axis aligned fills (rect fast path), overlapping
        for _ in range(500):
            o.append(rgb() + " rg %.2f %.2f %.2f %.2f re %s\n" % (rnd.uniform(-20, 612), rnd.uniform(-20, 792), rnd.uniform(0.1, 200), rnd.uniform(0.1, 200), rnd.choice(["f", "f*", "S", "B"])))
    else:  # thin nearly horizontal and nearly vertical lines (x-major and y-major edges)
        for _ in range(300):
            x, y = rnd.uniform(0, 612), rnd.uniform(0, 792)
            if rnd.random() < 0.5:
                o.append("%s RG %.2f w %.2f %.2f m %.2f %.2f l S\n" % (rgb(), rnd.uniform(0.05, 3), x, y, x + rnd.uniform(-600, 600), y + rnd.uniform(-2, 2)))
            else:
                o.append("%s RG %.2f w %.2f %.2f m %.2f %.2f l S\n" % (rgb(), rnd.uniform(0.05, 3), x, y, x + rnd.uniform(-2, 2), y + rnd.uniform(-700, 700)))
    return "".join(o).encode(), None

def shapes_resources(w):
    group = w.add(w.stream(b"q 0.3 g 0 0 612 792 re f 1 g 100 100 400 600 re f 0 g 200 200 m 400 300 l 300 500 l h f Q\n",
                           extra=b"/Type /XObject /Subtype /Form /BBox [0 0 612 792] /Group << /S /Transparency /CS /DeviceGray >> "))
    return (b"<< /Font << /F1 << /Type /Font /Subtype /Type1 /BaseFont /Helvetica >> >> "
            b"/ExtGState << /GS1 << /ca 0.45 /CA 0.7 >> /GS2 << /SMask << /S /Luminosity /G %d 0 R >> >> >> >>" % group)


def make_shapes(path):
    build_doc(path, 40, shapes_page, shapes_resources)


def make_images(out):
    w = PdfWriter()
    pages_id = w.reserve()
    rnd = random.Random(7)

    def image(width, height, cs, with_mask):
        n = {"DeviceGray": 1, "DeviceRGB": 3, "DeviceCMYK": 4}[cs]
        data = bytearray()
        for y in range(height):
            for x in range(width):
                for c in range(n):
                    v = (x * (c + 3) * 7 + y * (c + 1) * 5 + ((x // 7) ^ (y // 5)) * 31) & 255
                    data.append(v if rnd.random() > 0.05 else rnd.randrange(256))
        extra = b"/Type /XObject /Subtype /Image /Width %d /Height %d /ColorSpace /%s /BitsPerComponent 8 " % (width, height, cs.encode())
        if with_mask:
            m = bytes(((x * 255) // max(1, width - 1) + (y * 3)) & 255 for y in range(height) for x in range(width))
            mid = w.add(w.stream(m, extra=b"/Type /XObject /Subtype /Image /Width %d /Height %d /ColorSpace /DeviceGray /BitsPerComponent 8 " % (width, height)))
            extra += b"/SMask %d 0 R " % mid
        return w.add(w.stream(bytes(data), extra=extra))

    sizes = [(1, 40), (40, 1), (3, 3), (17, 9), (200, 150), (641, 480), (1203, 777), (2400, 1800)]
    spaces = ["DeviceGray", "DeviceRGB", "DeviceCMYK"]
    images = []
    for i, (iw, ih) in enumerate(sizes):
        for cs in spaces:
            images.append(image(iw, ih, cs, with_mask=(i % 2 == 1)))

    kids = []
    for p in range(24):
        prnd = random.Random(p)
        content = []
        xobjs = []
        for k in range(6):
            idx = prnd.randrange(len(images))
            name = b"Im%d" % k
            xobjs.append(b"/%s %d 0 R" % (name, images[idx]))
            sx = prnd.choice([1, 1, -1]) * prnd.choice([2.3, 17, 61, 100, 150, 233, 400, 612])
            sy = prnd.choice([1, 1, -1]) * prnd.choice([3.7, 25, 90, 150, 300, 700])
            tx = prnd.uniform(-100, 600)
            ty = prnd.uniform(-100, 780)
            content.append(b"q %.3f 0 0 %.3f %.2f %.2f cm /%s Do Q\n" % (sx, sy, tx, ty, name))
        res = b"<< /XObject << " + b" ".join(xobjs) + b" >> >>"
        cid = w.add(w.stream(b"".join(content)))
        pid = w.add(b"<< /Type /Page /Parent %d 0 R /MediaBox [0 0 612 792] /Resources %s /Contents %d 0 R >>" % (pages_id, res, cid))
        kids.append(pid)
    w.set(pages_id, b"<< /Type /Pages /Count %d /Kids [%s] >>" % (len(kids), b" ".join(b"%d 0 R" % k for k in kids)))
    root = w.add(b"<< /Type /Catalog /Pages %d 0 R >>" % pages_id)
    w.write(out, root)


def main():
    os.makedirs(OUT, exist_ok=True)
    for name, make in (("shapes_40.pdf", make_shapes), ("images_24.pdf", make_images)):
        p = os.path.join(OUT, name)
        if not os.path.exists(p):
            make(p)
            print("wrote", p)


if __name__ == "__main__":
    main()
