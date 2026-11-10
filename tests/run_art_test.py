#!/usr/bin/env python3
"""The album art maker's test: tests/run_art_test.py ARTTEST [--plain ARTTEST_PLAIN] [--tools DIR --samples DIR]
                                                              [--photo FILE]... [--cover FILE]... [--keep DIR]

Pictures are made up here (PNG written by hand, JPEG through sips), converted by arttest, and the textures decoded
here again: the header against the one written out below, the pixels against the picture scaled another way (a box
filter here; sips for comparison), probes for the channels' order, the rows' order and the cut of a picture that is
not square, the placeholder for what cannot be read. --plain is arttest built without the sanitizers: the timings,
the memory after many conversions, `leaks`, and the same bytes from another build. With --samples (and --tools,
rs-rearmed's tools/), the album art of every archive in that folder is compared with ours, header and size, and
used as pictures. --photo is any large picture sips can read (cut square and scaled to 1024 as a source), --cover a
picture used as it is (a feedpak's cover). Needs sips."""
import argparse, math, os, re, struct, subprocess, sys, tempfile, zlib

sys.dont_write_bytecode = True  # importing run_varc_test must leave nothing in tests/
checks = 0
EDGES = (64, 128, 256)
SIZES = {64: 2176, 128: 8320, 256: 32896}
DARK = (24, 24, 24)


def ok(cond, what):
    global checks
    checks += 1
    if not cond:
        sys.exit("FAIL: " + what)


def header(px):
    """The header of the game's own files (the official ones), written out independently of src/art.c"""
    h = bytearray(128)
    h[0:4] = b"DDS "
    struct.pack_into("<IIII", h, 4, 124, 0x1007, px, px)
    struct.pack_into("<II4s", h, 0x4c, 32, 4, b"DXT1")
    struct.pack_into("<I", h, 0x6c, 0x1000)
    return bytes(h)


# --- pictures: px * px * 3 bytes, the top row first ---------------------------------------------------------------
def png_write(path, w, h, rows, kind=2):
    """rows: h byte strings; kind 0 grey, 2 RGB, 6 RGBA (8 bits)"""
    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))
    raw = b"".join(b"\0" + r for r in rows)
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, kind, 0, 0, 0)) +
                           chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def png_read(path):
    """(w, h, RGB bytes) of an 8-bit PNG that is not interlaced, what is clear laid over black"""
    d = open(path, "rb").read()
    ok(d[:8] == b"\x89PNG\r\n\x1a\n", path + " is a PNG")
    p, idat, plte = 8, b"", None
    while p < len(d):
        n, tag = struct.unpack(">I4s", d[p:p + 8])
        body = d[p + 8:p + 8 + n]
        if tag == b"IHDR":
            w, h, depth, kind, _, _, lace = struct.unpack(">IIBBBBB", body)
        elif tag == b"IDAT":
            idat += body
        elif tag == b"PLTE":
            plte = body
        p += 12 + n
    ok(depth == 8 and lace == 0 and kind in (0, 2, 3, 4, 6), "%s: a PNG this reader knows (depth %d, kind %d)" % (path, depth, kind))
    bpp = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[kind]
    raw, stride = zlib.decompress(idat), w * bpp
    prev, out = bytearray(stride), bytearray()
    for y in range(h):
        f = raw[y * (stride + 1)]
        row = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        if f == 1:
            for i in range(bpp, stride):
                row[i] = (row[i] + row[i - bpp]) & 255
        elif f == 2:
            row = bytearray((a + b) & 255 for a, b in zip(row, prev))
        elif f == 3:
            for i in range(stride):
                row[i] = (row[i] + (((row[i - bpp] if i >= bpp else 0) + prev[i]) >> 1)) & 255
        elif f == 4:
            for i in range(stride):
                a, b, c = (row[i - bpp], prev[i], prev[i - bpp]) if i >= bpp else (0, prev[i], 0)
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                row[i] = (row[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        prev = row
        if kind == 2:
            out += row
        elif kind == 6:
            for i in range(0, stride, 4):
                al = row[i + 3]
                out += bytes(((row[i] * al + 127) // 255, (row[i + 1] * al + 127) // 255, (row[i + 2] * al + 127) // 255))
        elif kind == 3:
            for v in row:
                out += plte[3 * v:3 * v + 3]
        else:
            for i in range(0, stride, bpp):
                v = row[i] if bpp == 1 else (row[i] * row[i + 1] + 127) // 255
                out += bytes((v, v, v))
    return w, h, bytes(out)


def dds_decode(dds, px):
    """A DXT1 texture's pixels. A block whose first colour is not the larger number is the three-colour form; ours
    must never use its transparent index."""
    out = bytearray(px * px * 3)
    p = 128
    for by in range(0, px, 4):
        for bx in range(0, px, 4):
            c0, c1, bits = struct.unpack_from("<HHI", dds, p)
            p += 8
            pal = [((c >> 11) << 3 | (c >> 11) >> 2, (c >> 5 & 63) << 2 | (c >> 5 & 63) >> 4, (c & 31) << 3 | (c & 31) >> 2)
                   for c in (c0, c1)]
            if c0 > c1:
                pal.append(tuple((2 * a + b + 1) // 3 for a, b in zip(*pal)))
                pal.append(tuple((a + 2 * b + 1) // 3 for a, b in zip(pal[0], pal[1])))
            else:
                pal.append(tuple((a + b) // 2 for a, b in zip(*pal)))
                pal.append(None)
            for i in range(16):
                c = pal[bits >> (2 * i) & 3]
                if c is None:
                    return None  # a transparent pixel
                o = 3 * ((by + i // 4) * px + bx + i % 4)
                out[o:o + 3] = bytes(c)
    return bytes(out)


def box(rgb, w, f):
    """The picture w x w scaled down f times by averaging f x f pixels"""
    n = w // f
    sums = [0] * (n * n * 3)
    for y in range(w):
        row, base = rgb[y * w * 3:(y + 1) * w * 3], (y // f) * n * 3
        for c in range(3):
            ch = row[c::3]
            for x in range(n):
                sums[base + 3 * x + c] += sum(ch[x * f:(x + 1) * f])
    return bytes((2 * s + f * f) // (2 * f * f) for s in sums)


def psnr(a, b):
    ok(len(a) == len(b), "two pictures of one size")
    se = sum((x - y) * (x - y) for x, y in zip(a, b))
    return 99.0 if se == 0 else 10 * math.log10(255 * 255 * len(a) / se)


def at(rgb, px, fx, fy):
    o = 3 * (int(fy * px) * px + int(fx * px))
    return tuple(rgb[o:o + 3])


def near(c, want, tol=40):
    return all(abs(a - b) <= tol for a, b in zip(c, want))


def rgbx(rgb):
    out = bytearray(len(rgb) // 3 * 4)
    out[0::4], out[1::4], out[2::4] = rgb[0::3], rgb[1::3], rgb[2::3]
    return bytes(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("arttest")
    ap.add_argument("--plain")
    ap.add_argument("--tools")
    ap.add_argument("--samples")
    ap.add_argument("--photo", action="append", default=[])
    ap.add_argument("--cover", action="append", default=[])
    ap.add_argument("--srgb", default="/System/Library/ColorSync/Profiles/sRGB Profile.icc", help="the sRGB profile, for --photo")
    ap.add_argument("--keep", help="leave the pictures and textures in this folder")
    a = ap.parse_args()
    tmpdir = None if a.keep else tempfile.TemporaryDirectory()
    tmp = a.keep or tmpdir.name
    os.makedirs(tmp, exist_ok=True)
    T = lambda name: os.path.join(tmp, name)

    def run(*args, exe=None):
        return subprocess.run([exe or a.arttest] + [str(x) for x in args], capture_output=True, text=True)

    def sips(*args):
        r = subprocess.run(["sips"] + [str(x) for x in args], capture_output=True, text=True)
        ok(r.returncode == 0, "sips %s: %s" % (" ".join(map(str, args)), r.stderr))

    def convert(src, px, tag, exe=None):
        """(what arttest said, the texture, its pixels or None)"""
        out = T("%s_%d.dds" % (tag, px))
        r = run("dds", src, px, out, exe=exe)
        ok(r.returncode == 0, "%s at %d: arttest: %s%s" % (tag, px, r.stdout, r.stderr))
        ok("runtime error" not in r.stderr and "Sanitizer" not in r.stderr, "%s at %d: the sanitizers are quiet: %s" % (tag, px, r.stderr))
        dds = open(out, "rb").read()
        ok(len(dds) == SIZES[px], "%s at %d: %d bytes, not %d" % (tag, px, len(dds), SIZES[px]))
        ok(dds[:128] == header(px), "%s at %d: the header" % (tag, px))
        pix = dds_decode(dds, px)
        ok(pix is not None, "%s at %d: no transparent pixel" % (tag, px))
        return r.stdout.strip(), dds, pix

    # --- 1. the placeholder, and what cannot be read ---------------------------------------------------------------
    open(T("noise.bin"), "wb").write(bytes((i * 7919 + i // 3) & 255 for i in range(5000)))
    open(T("text.png"), "wb").write(b"this is not a picture\n" * 40)
    open(T("head.png"), "wb").write(b"\x89PNG\r\n\x1a\n" + b"\0\0\0\rIHDR")
    open(T("head.jpg"), "wb").write(b"\xff\xd8\xff\xe0\0\x10JFIF\0")
    plain = {}
    for px in EDGES:
        said, plain[px], pix = convert("-", px, "null")
        ok(said == "placeholder", "no picture: the placeholder")
        ok(pix == bytes(DARK) * (px * px), "the placeholder at %d is one dark grey" % px)
        for src in ("--", T("noise.bin"), T("text.png"), T("head.png"), T("head.jpg")):
            said, dds, _ = convert(src, px, "bad")
            ok(said == "placeholder" and dds == plain[px], "%s at %d: the placeholder" % (os.path.basename(src), px))
    print("placeholder: no picture, an empty one, noise, text, a PNG and a JPEG that stop after the signature")

    # --- 2. the channels and the rows: green band on top, below it red on the left and blue on the right ----------
    w = 512
    rows = [bytes((0, 255, 0)) * w if y < w // 4 else bytes((255, 0, 0)) * (w // 2) + bytes((0, 0, 255)) * (w // 2) for y in range(w)]
    png_write(T("rgb.png"), w, w, rows)
    sips("-s", "format", "jpeg", "-s", "formatOptions", "95", T("rgb.png"), "--out", T("rgb.jpg"))
    for src in ("rgb.png", "rgb.jpg"):
        for px in EDGES:
            said, _, pix = convert(T(src), px, "rgb")
            got = at(pix, px, .5, .125), at(pix, px, .25, .7), at(pix, px, .75, .7)
            ok(said == "picture" and near(got[0], (0, 255, 0)) and near(got[1], (255, 0, 0)) and near(got[2], (0, 0, 255)),
               "%s at %d: green on top, red left, blue right; got %s" % (src, px, got))
    print("channels and rows: green on top, red left, blue right, from PNG and JPEG at every size")

    # --- 3. other kinds of picture ----------------------------------------------------------------------------------
    # alpha: the left half clear (white underneath), the right half solid white
    png_write(T("alpha.png"), 64, 64, [bytes((255, 255, 255, 0)) * 32 + bytes((255, 255, 255, 255)) * 32] * 64, kind=6)
    # 16 x 16, four quarters
    q = [bytes((255, 255, 0)) * 8 + bytes((0, 255, 255)) * 8] * 8 + [bytes((255, 0, 255)) * 8 + bytes((255, 255, 255)) * 8] * 8
    png_write(T("tiny.png"), 16, 16, q)
    # grey: dark on top to light at the bottom
    png_write(T("grey.png"), 256, 256, [bytes((y,)) * 256 for y in range(256)], kind=0)
    sips("-s", "format", "jpeg", T("grey.png"), "--out", T("grey.jpg"))
    r = subprocess.run(["sips", "-g", "space", T("grey.jpg")], capture_output=True, text=True).stdout
    grey_jpeg = "Gray" in r
    # 3000 x 2000: the middle 2000 columns must be kept: green and yellow bands at their ends, red and blue outside,
    # white on the top tenth
    mid = lambda c: bytes((0, 255, 0)) * 200 + bytes(c) * 1600 + bytes((255, 255, 0)) * 200
    wide = lambda c: bytes((255, 0, 0)) * 500 + mid(c) + bytes((0, 0, 255)) * 500
    png_write(T("wide.png"), 3000, 2000, [wide((255, 255, 255))] * 200 + [wide((90, 90, 90))] * 1800)
    # and its transpose, 2000 x 3000
    tall = [bytes((255, 0, 0)) * 2000] * 500 + [bytes((0, 255, 0)) * 2000] * 200 + [bytes((255, 255, 255)) * 200 + bytes((90, 90, 90)) * 1800] * 1600 + \
           [bytes((255, 255, 0)) * 2000] * 200 + [bytes((0, 0, 255)) * 2000] * 500
    png_write(T("tall.png"), 2000, 3000, tall)
    r = run("cmyk", T("cmyk.jpg"))
    cmyk = r.returncode == 0
    if cmyk:
        r = subprocess.run(["sips", "-g", "space", T("cmyk.jpg")], capture_output=True, text=True).stdout
        cmyk = "CMYK" in r
    for px in EDGES:
        said, _, pix = convert(T("alpha.png"), px, "alpha")
        got = at(pix, px, .25, .5), at(pix, px, .75, .5)
        ok(said == "picture" and near(got[0], (0, 0, 0)) and near(got[1], (255, 255, 255)), "alpha at %d: clear is black; got %s" % (px, got))
        said, _, pix = convert(T("tiny.png"), px, "tiny")
        got = [at(pix, px, x, y) for y in (.25, .75) for x in (.25, .75)]
        ok(said == "picture" and all(near(g, w_) for g, w_ in zip(got, ((255, 255, 0), (0, 255, 255), (255, 0, 255), (255, 255, 255)))),
           "16 x 16 at %d: the four quarters; got %s" % (px, got))
        said, _, pix = convert(T("grey.jpg"), px, "grey")
        got = at(pix, px, .5, .1), at(pix, px, .5, .9)
        ok(said == "picture" and near(got[0], (26, 26, 26), 16) and near(got[1], (230, 230, 230), 16), "grey JPEG at %d: got %s" % (px, got))
        said, _, pix = convert(T("wide.png"), px, "wide")
        got = at(pix, px, .04, .5), at(pix, px, .96, .5), at(pix, px, .5, .04), at(pix, px, .5, .5)
        ok(said == "picture" and near(got[0], (0, 255, 0)) and near(got[1], (255, 255, 0)) and near(got[2], (255, 255, 255)) and near(got[3], (90, 90, 90)),
           "3000 x 2000 at %d: the middle square; got %s" % (px, got))
        said, _, pix = convert(T("tall.png"), px, "tall")
        got = at(pix, px, .5, .04), at(pix, px, .5, .96), at(pix, px, .04, .5), at(pix, px, .5, .5)
        ok(said == "picture" and near(got[0], (0, 255, 0)) and near(got[1], (255, 255, 0)) and near(got[2], (255, 255, 255)) and near(got[3], (90, 90, 90)),
           "2000 x 3000 at %d: the middle square; got %s" % (px, got))
        if cmyk:
            said, _, pix = convert(T("cmyk.jpg"), px, "cmyk")
            # cyan grows to the right, magenta downwards, yellow on the right half: the corners differ and none is inverted to dark
            got = at(pix, px, .05, .05), at(pix, px, .95, .05), at(pix, px, .05, .95)
            ok(said == "picture" and got[0][0] > got[1][0] + 60 and got[0][1] > got[2][1] + 60 and min(got[0]) > 150,
               "CMYK JPEG at %d: got %s" % (px, got))
    print("pictures: PNG with alpha, 16 x 16, %s JPEG, 3000 x 2000 and 2000 x 3000 cut to the middle square%s"
          % ("grey" if grey_jpeg else "grey (sips wrote it as colour)", ", CMYK JPEG" if cmyk else "; NO CMYK JPEG could be made"))
    # a PNG cut in half: either a picture or the placeholder, but a whole texture
    d = open(T("rgb.png"), "rb").read()
    open(T("half.png"), "wb").write(d[:len(d) // 2])
    print("half a PNG:", convert(T("half.png"), 64, "half")[0])

    # --- 4. the game's own files -----------------------------------------------------------------------------------
    sources = []  # (name, path of a square picture, its edge)
    if a.samples and a.tools:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import run_varc_test as v
        px_ = v.reader(a.tools, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src", "keys.h"))
        known = set(range(0x08, 0x0c)) | set(range(0x14, 0x18)) | set(range(0x1c, 0x20)) | set(range(0x20, 0x4c))
        seen, n = {}, 0
        for name in sorted(os.listdir(a.samples)):
            if not name.endswith(".psarc"):
                continue
            for entry, data, _ in v.read_all(px_, os.path.join(a.samples, name)):
                m = re.match(r"gfxassets/album_art/album_.*_(\d+)\.dds$", entry)
                if not m:
                    continue
                px, n = int(m.group(1)), n + 1
                ok(px in SIZES and len(data) == SIZES[px], "%s: %d bytes, ours has %d" % (entry, len(data), SIZES.get(px, 0)))
                diff = [i for i in range(128) if data[i] != header(px)[i]]
                ok(set(diff) <= known, "%s: its header differs from ours at %s" % (entry, [hex(i) for i in diff if i not in known]))
                form = "flags %#x, size field %d, levels %d, reserved %r" % (struct.unpack_from("<I", data, 8)[0], struct.unpack_from("<I", data, 0x14)[0],
                                                                             struct.unpack_from("<I", data, 0x1c)[0], data[0x20:0x4c].rstrip(b"\0"))
                seen.setdefault(form, []).append(name)
                if px == 256:
                    pix = dds_decode(data, 256)
                    if pix is not None:
                        p = T("real%d.png" % len(sources))
                        png_write(p, 256, 256, [pix[y * 768:(y + 1) * 768] for y in range(256)])
                        sources.append(("the game's " + name[:24], p, 256))
        ok(n > 0, "album art in the samples")
        print("%d album art files in the samples: every size as ours, the headers as ours but for:" % n)
        for form, names in sorted(seen.items()):
            print("   %s: %s" % (form, ", ".join(sorted(set(names)))))

    # --- 5. quality ------------------------------------------------------------------------------------------------
    # a made-up picture with slopes, edges and texture
    w = 1024
    rows = []
    for y in range(w):
        row = bytearray(w * 3)
        for x in range(w):
            t = (x * 131 + y * 71) % 256 if (x // 128 + y // 128) % 2 else 128 + int(100 * math.sin(x / 37.0) * math.cos(y / 53.0))
            row[3 * x], row[3 * x + 1], row[3 * x + 2] = x // 4, max(0, min(255, t)), 255 - y // 4
        rows.append(bytes(row))
    png_write(T("made.png"), w, w, rows)
    sources.insert(0, ("made-up", T("made.png"), 1024))
    sides = []
    for i, photo in enumerate(a.photo):
        r = subprocess.run(["sips", "-g", "pixelWidth", "-g", "pixelHeight", photo], capture_output=True, text=True).stdout
        side = min(int(x) for x in re.findall(r"pixel(?:Width|Height): (\d+)", r))
        sides.append(side)
        sips("-c", side, side, "-s", "format", "png", photo, "--out", T("photo%d_sq.png" % i))
        # into sRGB, as the reader here knows nothing of colour profiles (ours turns any profile into sRGB)
        sips("-z", 1024, 1024, "--matchTo", a.srgb, T("photo%d_sq.png" % i), "--out", T("photo%d.png" % i))
        sources.append(("photo " + os.path.basename(photo), T("photo%d.png" % i), 1024))
    for i, cover in enumerate(a.cover):
        sips("-s", "format", "png", cover, "--out", T("cover%d.png" % i))
        cw, ch, _ = png_read(T("cover%d.png" % i))
        if cw == ch and cw in (256, 512, 1024):
            sources.append(("cover " + os.path.basename(cover), T("cover%d.png" % i), cw))
        else:
            print("cover %s is %d x %d: not compared (converted below all the same)" % (os.path.basename(cover), cw, ch))
            for px in EDGES:
                ok(convert(cover, px, "cover%d" % i)[0] == "picture", "the cover at %d" % px)
    print("PSNR in dB at 64, 128, 256: ours against a box filter's scaling | against sips' scaling | the compressor alone on the box filter's")
    for name, path, edge in sources:
        sw, sh, src = png_read(path)
        ok(sw == edge and sh == edge, name + ": the size expected")
        if sw > 256:
            src = box(src, sw, sw // 256)
        ref = {256: src, 128: box(src, 256, 2), 64: box(src, 256, 4)}
        cols = [[], [], []]
        for px in EDGES:
            said, _, pix = convert(path, px, "q")
            ok(said == "picture", name + ": read")
            cols[0].append(psnr(pix, ref[px]))
            sips("-z", px, px, path, "--out", T("sips.png"))
            cols[1].append(psnr(pix, png_read(T("sips.png"))[2]))
            open(T("ref.rgbx"), "wb").write(rgbx(ref[px]))
            r = run("dxt1", T("ref.rgbx"), px, T("ref.dxt"))
            ok(r.returncode == 0, "the compressor alone: " + r.stderr)
            alone = dds_decode(header(px) + open(T("ref.dxt"), "rb").read(), px)
            ok(alone is not None, "the compressor alone: no transparent pixel")
            cols[2].append(psnr(alone, ref[px]))
        print("   %-36s %s" % (name, " | ".join(" ".join("%5.1f" % x for x in c) for c in cols)))
        # covers of 256 pixels are sharp at that size (lettering, hard edges): two or four pixels to one leaves
        # detail that no DXT1 block holds, so they are held to less
        floor = 27 if edge == 256 else 30
        if name.startswith("cover"):  # a cover given on the command line may be as busy as it likes: only that it is the picture
            floor = 20
        ok(min(cols[2]) > floor, "%s: the compressor alone keeps %d dB" % (name, floor))
        ok(min(cols[1]) > floor, "%s: %d dB against sips' scaling" % (name, floor))
        if name.startswith("photo"):
            ok(min(cols[0]) > 30, name + ": 30 dB against the box filter's scaling")

    # --- 6. the same bytes again: another process, another build, many rounds, many threads; memory; time -----------
    pictures = [T("rgb.jpg"), T("made.png"), T("wide.png"), T("alpha.png")] + [p for _, p, _ in sources[1:]] + (a.cover or []) + \
        ([T("cmyk.jpg")] if cmyk else [])
    for p in pictures:
        for px in EDGES:
            one, two = convert(p, px, "det1")[1], convert(p, px, "det2")[1]
            ok(one == two, "%s at %d: the same bytes from two processes" % (p, px))
            if a.plain:
                ok(convert(p, px, "det3", exe=a.plain)[1] == one, "%s at %d: the same bytes from the build without sanitizers" % (p, px))
    print("the same bytes from two processes%s: %d pictures at every size" % (" and from both builds" if a.plain else "", len(pictures)))
    r = run("threads", T("made.png"), 10)
    ok(r.returncode == 0, "threads: %s%s" % (r.stdout, r.stderr))
    print("threads:", r.stdout.strip())
    r = run("fuzz", T("rgb.jpg"), 300)
    ok(r.returncode == 0, "damaged JPEGs: %s%s" % (r.stdout, r.stderr[-2000:]))
    print("JPEG,", r.stdout.strip())
    r = run("fuzz", T("made.png"), 150)
    ok(r.returncode == 0, "damaged PNGs: %s%s" % (r.stdout, r.stderr[-2000:]))
    print("PNG,", r.stdout.strip())
    r = run("loop", T("rgb.jpg"), 256, 300)
    ok(r.returncode == 0, "300 rounds under the sanitizers (which look for leaks at the end): %s%s" % (r.stdout, r.stderr[-2000:]))
    if a.plain:
        big, what = T("wide.png"), "a 3000 x 2000 PNG"
        if a.photo:  # the whole photo as a JPEG, as large as it is
            big = T("big.jpg")
            sips("-s", "format", "jpeg", T("photo0_sq.png"), "--out", big)
            what = "a %d x %d JPEG" % (sides[0], sides[0])
        print("time for one conversion (and the memory gained from the first tenth of the rounds to the last):")
        for label, p in (("a 1024 x 1024 PNG", T("made.png")), ("a 512 x 512 JPEG", T("rgb.jpg")), ("no picture", "-"), ("noise", T("noise.bin")), (what, big)):
            line = []
            for px in EDGES:
                rounds = 40 if p == big else 400
                r = run("loop", p, px, rounds, exe=a.plain)
                ok(r.returncode == 0, "loop %s: %s%s" % (label, r.stdout, r.stderr))
                ms, early, late = re.match(r"([\d.]+) ms each, (\d+) KiB after \d+, (\d+) KiB", r.stdout).groups()
                ok(int(late) - int(early) < 2048, "%s at %d: memory grew from %s to %s KiB over %d rounds" % (label, px, early, late, rounds))
                line.append("%s ms (%+d KiB)" % (ms, int(late) - int(early)))
            print("   %-28s 64: %s, 128: %s, 256: %s" % (label, *line))
        r = subprocess.run(["leaks", "-q", "--atExit", "--", a.plain, "loop", T("rgb.jpg"), "256", "200"], capture_output=True, text=True)
        m = re.search(r"(\d+) leaks? for (\d+) total leaked bytes", r.stdout)
        if m:
            ok(m.group(1) == "0", "leaks: " + m.group(0))
            print("leaks --atExit after 200 conversions:", m.group(0))
        else:
            print("leaks could not look (exit %d): NOT checked that way" % r.returncode)
    print("ok, %d checks" % checks)


if __name__ == "__main__":
    main()
