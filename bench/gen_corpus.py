#!/usr/bin/env python3
"""Generate a deterministic corpus of large PDFs for benchmarking sioyek.

  text_4000.pdf    4000 pages of dense "academic" text (sections, figures, equations, references)
  scan_300.pdf     300 pages, each a 2480x3508 JPEG (300 dpi A4 scan)
  vector_200.pdf   200 pages with ~25k stroked path segments each
"""
import io
import os
import random
import sys
import zlib

OUT = sys.argv[1] if len(sys.argv) > 1 else "."


class PdfWriter:
    def __init__(self):
        self.objects = []  # bytes of each object body, 1-based ids

    def reserve(self):
        self.objects.append(None)
        return len(self.objects)

    def set(self, oid, body):
        self.objects[oid - 1] = body

    def add(self, body):
        oid = self.reserve()
        self.set(oid, body)
        return oid

    def stream(self, data, extra=b"", compress=True):
        if compress:
            data = zlib.compress(data, 6)
            extra = b"/Filter /FlateDecode " + extra
        return b"<< " + extra + b"/Length %d >>\nstream\n" % len(data) + data + b"\nendstream"

    def write(self, path, root_id):
        with open(path, "wb") as f:
            f.write(b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n")
            offsets = []
            for i, body in enumerate(self.objects):
                offsets.append(f.tell())
                f.write(b"%d 0 obj\n" % (i + 1))
                f.write(body)
                f.write(b"\nendobj\n")
            xref = f.tell()
            f.write(b"xref\n0 %d\n" % (len(self.objects) + 1))
            f.write(b"0000000000 65535 f \n")
            for off in offsets:
                f.write(b"%010d 00000 n \n" % off)
            f.write(b"trailer\n<< /Size %d /Root %d 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (len(self.objects) + 1, root_id, xref))


def build_doc(path, num_pages, page_content_fn, resources, media=(612, 792)):
    w = PdfWriter()
    pages_id = w.reserve()
    res_id = w.add(resources(w) if callable(resources) else resources)
    kids = []
    for i in range(num_pages):
        content, extra_res = page_content_fn(w, i)
        cid = w.add(w.stream(content))
        res_ref = b"%d 0 R" % res_id if extra_res is None else extra_res
        pid = w.add(b"<< /Type /Page /Parent %d 0 R /MediaBox [0 0 %d %d] /Resources %s /Contents %d 0 R >>"
                    % (pages_id, media[0], media[1], res_ref, cid))
        kids.append(pid)
    w.set(pages_id, b"<< /Type /Pages /Count %d /Kids [%s] >>" % (num_pages, b" ".join(b"%d 0 R" % k for k in kids)))
    root = w.add(b"<< /Type /Catalog /Pages %d 0 R >>" % pages_id)
    w.write(path, root)


WORDS = ("the of and to in is that for with as on by this are be we an which from at it or our model "
         "results method data analysis function value system theorem proof lemma given where each "
         "network learning distribution parameter matrix vector space linear convex optimal bound "
         "algorithm sample error estimate approach problem section figure table equation shows "
         "however therefore moreover consider following previous proposed experimental performance "
         "significant representation structure boundary condition solution numerical simulation").split()
CAPS = ["Figure", "Table", "Section", "Theorem", "Lemma", "Definition", "Algorithm", "Corollary"]


def esc(s):
    return s.replace("\\", "\\\\").replace("(", "\\(").replace(")", "\\)")


def text_page(w, i):
    rnd = random.Random(i)
    out = io.StringIO()
    y = 740
    out.write("BT\n")
    paper_page = i % 12
    if paper_page == 0:
        out.write("/F2 16 Tf 1 0 0 1 72 %d Tm (%s) Tj\n" % (y, esc("A Study of %s %s" % (rnd.choice(WORDS), rnd.choice(WORDS)))))
        y -= 30
    sec = 1 + (i % 12) // 2
    out.write("/F2 12 Tf 1 0 0 1 72 %d Tm (%s) Tj\n" % (y, esc("%d.%d %s %s" % (sec, i % 5 + 1, rnd.choice(WORDS).capitalize(), rnd.choice(WORDS)))))
    y -= 22
    references = paper_page == 11
    ref_no = 1
    while y > 60:
        r = rnd.random()
        if references:
            txt = "[%d] A. %s and B. %s, %s %s %s, Journal of %s, %d." % (
                ref_no, rnd.choice(WORDS).capitalize(), rnd.choice(WORDS).capitalize(), rnd.choice(WORDS),
                rnd.choice(WORDS), rnd.choice(WORDS), rnd.choice(WORDS).capitalize(), 1990 + rnd.randrange(35))
            ref_no += 1
            out.write("/F1 9 Tf 1 0 0 1 72 %d Tm (%s) Tj\n" % (y, esc(txt)))
            y -= 12
            continue
        if r < 0.05:
            out.write("/F1 10 Tf 1 0 0 1 150 %d Tm (%s) Tj\n" % (y, esc("f(x) = sum x_i w_i + b")))
            out.write("/F1 10 Tf 1 0 0 1 500 %d Tm (%s) Tj\n" % (y, esc("(%d.%d)" % (sec, rnd.randrange(1, 40)))))
            y -= 18
            continue
        if r < 0.09:
            txt = "%s %d.%d: %s %s %s %s." % (rnd.choice(CAPS), sec, rnd.randrange(1, 9), rnd.choice(WORDS).capitalize(),
                                             rnd.choice(WORDS), rnd.choice(WORDS), rnd.choice(WORDS))
            out.write("/F1 10 Tf 1 0 0 1 72 %d Tm (%s) Tj\n" % (y, esc(txt)))
            y -= 13
            continue
        n = 0
        words = []
        while n < 88:
            wd = rnd.choice(WORDS)
            if rnd.random() < 0.03:
                wd = "[%d]" % rnd.randrange(1, 60)
            words.append(wd)
            n += len(wd) + 1
        line = " ".join(words)
        if rnd.random() < 0.1:
            line += " hyph-"
        out.write("/F1 10 Tf 1 0 0 1 72 %d Tm (%s) Tj\n" % (y, esc(line)))
        y -= 12
    out.write("ET\n")
    out.write("BT /F1 9 Tf 1 0 0 1 300 30 Tm (%d) Tj ET\n" % (i + 1))
    return out.getvalue().encode("latin-1"), None


def text_resources(w):
    f1 = w.add(b"<< /Type /Font /Subtype /Type1 /BaseFont /Times-Roman /Encoding /WinAnsiEncoding >>")
    f2 = w.add(b"<< /Type /Font /Subtype /Type1 /BaseFont /Times-Bold /Encoding /WinAnsiEncoding >>")
    return b"<< /Font << /F1 %d 0 R /F2 %d 0 R >> >>" % (f1, f2)


def make_scan_images(n):
    from PIL import Image, ImageDraw, ImageFilter
    import numpy as np
    blobs = []
    W, H = 2480, 3508
    for k in range(n):
        rnd = random.Random(1000 + k)
        img = Image.new("L", (W, H), 245)
        d = ImageDraw.Draw(img)
        y = 250
        while y < H - 250:
            x = 250
            while x < W - 250:
                ww = rnd.randrange(30, 160)
                d.rectangle([x, y, x + ww, y + 32], fill=rnd.randrange(10, 60))
                x += ww + rnd.randrange(18, 30)
            y += 62
        arr = np.asarray(img.filter(ImageFilter.GaussianBlur(1.2)), dtype=np.int16)
        arr = arr + np.random.default_rng(k).integers(-12, 12, size=arr.shape, dtype=np.int16)
        img = Image.fromarray(np.clip(arr, 0, 255).astype(np.uint8), "L")
        buf = io.BytesIO()
        img.save(buf, "JPEG", quality=80)
        blobs.append(buf.getvalue())
    return blobs, W, H


def main():
    os.makedirs(OUT, exist_ok=True)

    p = os.path.join(OUT, "text_4000.pdf")
    if not os.path.exists(p):
        build_doc(p, 4000, text_page, text_resources)
        print("wrote", p)

    p = os.path.join(OUT, "scan_300.pdf")
    if not os.path.exists(p):
        blobs, W, H = make_scan_images(10)

        def scan_page(w, i):
            blob = blobs[i % len(blobs)]
            img = w.add(w.stream(blob, b"/Type /XObject /Subtype /Image /Width %d /Height %d /ColorSpace /DeviceGray "
                                       b"/BitsPerComponent 8 /Filter /DCTDecode " % (W, H), compress=False))
            return b"q 595 0 0 842 0 0 cm /Im0 Do Q", b"<< /XObject << /Im0 %d 0 R >> >>" % img

        build_doc(p, 300, scan_page, b"<< >>", media=(595, 842))
        print("wrote", p)

    p = os.path.join(OUT, "vector_200.pdf")
    if not os.path.exists(p):
        def vector_page(w, i):
            rnd = random.Random(i)
            out = io.StringIO()
            for series in range(25):
                out.write("%.3f %.3f %.3f RG %.2f w\n" % (rnd.random(), rnd.random(), rnd.random(), 0.3 + rnd.random()))
                x, y = 40.0, 100 + rnd.random() * 600
                out.write("%.2f %.2f m\n" % (x, y))
                for _ in range(1000):
                    x += 0.53
                    y += rnd.uniform(-6, 6)
                    out.write("%.2f %.2f l\n" % (x, y))
                out.write("S\n")
            for _ in range(300):
                out.write("%.3f %.3f %.3f rg %.1f %.1f %.1f %.1f re f\n" % (
                    rnd.random(), rnd.random(), rnd.random(), rnd.uniform(40, 560), rnd.uniform(40, 750), rnd.uniform(2, 20), rnd.uniform(2, 20)))
            return out.getvalue().encode(), None

        build_doc(p, 200, vector_page, b"<< >>")
        print("wrote", p)


if __name__ == "__main__":
    main()
