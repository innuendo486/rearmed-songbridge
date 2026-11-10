#!/usr/bin/env python3
"""Checks the mod on real archives, without a game.

usage: build.sh --test [--all] [--samples DIR] [--converter psarc_to_mac.py] [--plain X_p.psarc]

SBTEST (tests/sbtest.c) starts the mod for one edition and reads through rc_file.c, as the game does. For each
sample this compares the archive the mod serves with what the offline converter makes of the same file (every
entry's content; a song file's after decrypting it with the edition's key), and, where the samples hold the same
song for both platforms, with the other platform's real archive. Then the listing rules: one archive a song, and
files that are not whole archives left alone.

Nothing is written next to the samples: the folders the mod sees are temporary, with links to them.
"""
import argparse
import collections
import hashlib
import importlib.util
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

BLOCK = 0x10000
SFX = {"mac-x64": "_m.psarc", "win-x86": "_p.psarc"}
OTHER = {"mac-x64": "win-x86", "win-x86": "mac-x64"}
KEY = {"mac-x64": "mac", "win-x86": "pc"}
conv = None
sbtest = None
checks = 0


def ok(cond, what):
    global checks
    checks += 1
    if not cond:
        sys.exit("FAIL: " + what)


def sb(target, *args):
    r = subprocess.run([sbtest, target] + list(args), capture_output=True, text=True)
    if r.returncode:
        sys.exit("FAIL: sbtest %s %s: exit %d\n%s" % (target, " ".join(args), r.returncode, r.stderr))
    return r.stdout


def listing(target, d):
    return sorted(sb(target, "list", d).split("\n")[:-1])


def looks_zlib(b):
    return len(b) >= 2 and b[0] & 15 == 8 and b[0] >> 4 <= 7 and (b[0] << 8 | b[1]) % 31 == 0


def strict(path):
    """The archive read as a strict reader does: every entry inside the file, a block compressed exactly when it is
    shorter than 64 KiB and starts with a zlib header. names -> content, in the table's order."""
    d = open(path, "rb").read()
    magic, ver, comp, toc_len, esize, n, bsize, flags = struct.unpack(">4sI4sIIIII", d[:32])
    ok(magic == b"PSAR" and ver == 0x10004 and comp == b"zlib" and esize == 30 and bsize == BLOCK, "header")
    ok(flags & 4, "table of contents encrypted")
    toc = conv.aes("cfb", conv.TOC_KEY, conv.TOC_IV, d[32:toc_len], True)
    ok((len(toc) - 30 * n) % 2 == 0, "table of contents length")
    zl = struct.unpack(">%dH" % ((len(toc) - 30 * n) // 2), toc[30 * n:])
    files, md5s, used = [], [], 0
    for i in range(n):
        e = toc[30 * i:30 * i + 30]
        zi, length, off = struct.unpack(">I", e[16:20])[0], int.from_bytes(e[20:25], "big"), int.from_bytes(e[25:30], "big")
        if i == 0:
            ok(off == toc_len, "the name list follows the table of contents")
        out = b""
        ok(zi == used, "blocks in order")
        while len(out) < length:
            z = zl[zi] or BLOCK
            ok(toc_len <= off and off + z <= len(d), "entry %d inside the file" % i)
            chunk = d[off:off + z]
            want = min(BLOCK, length - len(out))
            if z < BLOCK and looks_zlib(chunk):
                chunk = zlib.decompress(chunk)
            ok(len(chunk) == want, "entry %d block size" % i)
            out += chunk
            off += z
            zi += 1
        used = zi
        files.append(out)
        md5s.append(e[:16])
    ok(used == len(zl), "no block size unused")
    names = files[0].decode().split("\n")
    ok(len(names) == n - 1, "one name an entry")
    ok(md5s[0] == bytes(16), "name list md5")
    for name, m in zip(names, md5s[1:]):
        ok(hashlib.md5(name.encode()).digest() == m, "md5 of " + name)
    return collections.OrderedDict(zip(names, files[1:]))


def is_sng(name):
    return "songs/bin/generic/" in name or "songs/bin/macos/" in name


def sng_plain(name, data):
    key = conv.SNG_KEY["mac" if "/macos/" in name else "pc"]
    return conv.sng_payload(data, key)


def to_win(s):
    return s.replace("audio/mac", "audio/windows").replace("bin/macos", "bin/generic")


def lines(graph):
    return collections.Counter(re.sub(r"urn:uuid:[0-9a-f-]+", "U", l) for l in graph.decode().splitlines())


MAC_SNG_TAG = '<U> <http://emergent.net/aweb/1.0/tag> "macos".'


def served(target, src, tmp):
    """src (the other platform's) as target's edition reads it: its content by name"""
    base = os.path.basename(src)
    stem = base[:-len("_m.psarc")]
    d = tempfile.mkdtemp(dir=tmp)
    dlc = os.path.join(d, "dlc")
    os.mkdir(dlc)
    os.symlink(src, os.path.join(dlc, base))
    vname = stem + SFX[target]
    ok(listing(target, dlc) == [vname], "%s listed as %s alone" % (base, vname))
    out = os.path.join(d, "out.psarc")
    sb(target, "dump", os.path.join(dlc, vname), out)
    content = strict(out)
    lenient = conv.read_psarc(out)[1]
    ok(list(lenient.items()) == list(content.items()), "the converter's reader reads the same")
    size = os.path.getsize(out)
    shutil.rmtree(d)
    return content, size


def check_sng(name, data, want_payload):
    magic, flags = struct.unpack("<II", data[:8])
    ok(magic == 0x4a and flags == 3, name + ": an encrypted song file")
    ok(data[-56:] == bytes(56), name + ": signature zero")
    ok(sng_plain(name, data) == want_payload, name + ": decrypts to the same song")


def win_to_mac(p, twin, tmp):
    got, size = served("mac-x64", p, tmp)
    src = conv.read_psarc(p)[1]
    ok(list(got) == [conv.to_mac(k) for k in src], "names")
    nsng = 0
    for name, data in src.items():
        v = got[conv.to_mac(name)]
        if is_sng(name):
            check_sng(conv.to_mac(name), v, sng_plain(name, data))
            ok(len(v) == len(data), name + ": length kept")
            nsng += 1
        elif name.endswith("aggregategraph.nt"):
            ok(v == conv.to_mac(data.decode()).replace("dx9", "macos").encode(), "aggregate graph as the converter's")
        else:
            ok(v == data, name + ": unchanged")
    note, differ = "", []
    if twin:
        real = conv.read_psarc(twin)[1]
        ok(list(real) == list(got), "names as the real Mac archive's")
        for name, data in real.items():
            if is_sng(name):
                ok(sng_plain(name, data) == sng_plain(name, got[name]), name + ": the real Mac archive's song")
            elif name.endswith("aggregategraph.nt"):
                diff = lines(data) - lines(got[name])
                ok(not (lines(got[name]) - lines(data)) and set(diff) <= {MAC_SNG_TAG}, "aggregate graph: the real one's lines but its song files' macos tags")
            elif data != got[name]:
                # the platforms' real archives differ in this file themselves: not the mod's doing (it serves the
                # source's bytes, checked above)
                ok(not name.startswith(("manifests/", "gamexblocks/", "songs/")), name + ": the real Mac archive's")
                differ.append(name)
        note = ", matches " + os.path.basename(twin) + (" but its own " + ", ".join(differ) if differ else "")
    print("  mac-x64  %-34s %9d bytes, %2d entries, %d song files%s" % (os.path.basename(p), size, len(got), nsng, note))


def mac_to_win(m, twin, tmp):
    got, size = served("win-x86", m, tmp)
    src = conv.read_psarc(m)[1]
    ok(list(got) == [to_win(k) for k in src], "names")
    nsng = 0
    for name, data in src.items():
        w = to_win(name)
        v = got[w]
        ok("macos" not in w and "audio/mac" not in w, w + ": a Windows name")
        if is_sng(name):
            check_sng(w, v, sng_plain(name, data))
            ok(len(v) == len(data), name + ": length kept")
            nsng += 1
        elif name.endswith("aggregategraph.nt"):
            ok(b"macos" not in v and b"audio/mac" not in v, "aggregate graph: nothing of the Mac's")
            ok(conv.to_mac(v.decode()).replace("dx9", "macos").encode() != v, "aggregate graph: Windows tags")
            # back through the converter: the Mac's graph but its song files' macos tags
            back = lines(conv.to_mac(v.decode()).replace("dx9", "macos").encode())
            diff = lines(data) - back
            ok(not (back - lines(data)) and set(diff) <= {MAC_SNG_TAG}, "aggregate graph: converts back")
        else:
            ok(v == data, name + ": unchanged")
    note, differ = "", []
    if twin:
        real = conv.read_psarc(twin)[1]
        ok(list(real) == list(got), "names as the real Windows archive's")
        for name, data in real.items():
            if is_sng(name):
                ok(sng_plain(name, data) == sng_plain(name, got[name]), name + ": the real Windows archive's song")
            elif name.endswith("aggregategraph.nt"):
                ok(lines(data) == lines(got[name]), "aggregate graph: the real Windows archive's lines")
            elif data != got[name]:
                # the platforms' real archives differ in this file themselves: not the mod's doing (it serves the
                # source's bytes, checked above)
                ok(not name.startswith(("manifests/", "gamexblocks/", "songs/")), name + ": the real Windows archive's")
                differ.append(name)
        note = ", matches " + os.path.basename(twin) + (" but its own " + ", ".join(differ) if differ else "")
    print("  win-x86  %-34s %9d bytes, %2d entries, %d song files%s" % (os.path.basename(m), size, len(got), nsng, note))


def rules(target, foreign, native, tmp):
    """The listing: foreign is an archive of the other platform, native the same song for target's"""
    fs, ns = SFX[OTHER[target]], SFX[target]
    n = [0]

    def folder(*files):
        n[0] += 1
        dlc = os.path.join(tmp, "rules-%s-%d" % (target, n[0]), "dlc")
        os.makedirs(dlc)
        for rel, src in files:
            os.makedirs(os.path.dirname(os.path.join(dlc, rel)), exist_ok=True)
            if isinstance(src, bytes):
                open(os.path.join(dlc, rel), "wb").write(src)
            else:
                os.symlink(src, os.path.join(dlc, rel))
        return dlc

    def stat(p):
        return sb(target, "stat", p).split()[0]

    real_size = os.path.getsize(native)
    # both forms under one name: the edition's own, as a file
    dlc = folder(("a" + fs, foreign), ("a" + ns, native))
    ok(listing(target, dlc) == ["a" + ns], "both forms: one name")
    ok(sb(target, "stat", dlc + "/a" + ns).split()[:2] == ["real", str(real_size)], "both forms: the edition's own file is read")
    # the same song under another name, in a subfolder: by the song key
    dlc = folder(("a" + fs, foreign), ("sub/other" + ns, native))
    ok(listing(target, dlc) == ["sub"], "same song elsewhere: the other platform's is not shown")
    ok(listing(target, dlc + "/sub") == ["other" + ns], "same song elsewhere: the edition's own is")
    ok(stat(dlc + "/a" + ns) == "none", "same song elsewhere: not served")
    # and the other way round: the edition's own above, the other platform's below
    dlc = folder(("sub/a" + fs, foreign), ("other" + ns, native))
    ok(listing(target, dlc) == ["other" + ns, "sub"] and listing(target, dlc + "/sub") == [], "same song above: not shown")
    # the same song twice in the other platform's form: once
    dlc = folder(("a" + fs, foreign), ("b" + fs, foreign), ("sub/c" + fs, foreign))
    ok(listing(target, dlc) == ["a" + ns, "sub"] and listing(target, dlc + "/sub") == [], "the same song three times: shown once")
    ok(stat(dlc + "/a" + ns) == "served" and stat(dlc + "/b" + ns) == "none", "the same song three times: served once")
    # in a subfolder, and with the suffix in capitals
    dlc = folder(("sub/deep/a" + fs.upper(), foreign))
    ok(listing(target, dlc + "/sub/deep") == ["a" + ns], "subfolder: listed")
    ok(stat(dlc + "/sub/deep/a" + ns) == "served" and stat(dlc + "/SUB/deep/A" + ns.upper()) == "served", "subfolder: served, whatever the case")
    ok(stat(dlc + "/sub/deep/a" + fs.upper()) == "real", "the file itself still opens")
    # not archives: left alone, next to one that is
    whole = open(foreign, "rb").read()
    toc_len = struct.unpack(">I", whole[12:16])[0]
    bad = {"cut": whole[:len(whole) * 3 // 5], "cutlast": whole[:-1], "cuttoc": whole[:32 + (toc_len - 32) // 2],
           "head": whole[:20], "empty": b"", "noise": os.urandom(100000), "magic": b"PSAR" + os.urandom(5000),
           "badtoc": whole[:40] + os.urandom(toc_len - 40) + whole[toc_len:],
           "text": b"not an archive\n"}
    dlc = folder(("a" + fs, foreign), *[(k + fs, v) for k, v in bad.items()])
    ok(listing(target, dlc) == sorted(["a" + ns] + [k + fs for k in bad]), "not archives: listed as they are")
    for k, v in bad.items():
        ok(stat(dlc + "/" + k + ns) == "none", k + ": not served")
        ok(sb(target, "stat", dlc + "/" + k + fs).split()[:2] == ["real", str(len(v))], k + ": the file as it is")
    ok(stat(dlc + "/a" + ns) == "served", "the archive next to them is served")
    # outside a dlc folder: nothing
    d = os.path.join(tmp, "elsewhere-" + target)
    os.mkdir(d)
    os.symlink(foreign, d + "/a" + fs)
    ok(listing(target, d) == ["a" + fs] and stat(d + "/a" + ns) == "none", "outside dlc: untouched")
    # an edition's own archives alone: nothing changes
    dlc = folder(("a" + ns, native), ("sub/b" + ns, native))
    ok(listing(target, dlc) == ["a" + ns, "sub"] and stat(dlc + "/a" + ns) == "real", "own archives alone: untouched")
    print("  %s  listing rules, files that are not archives" % target)


def main():
    global conv, sbtest
    ap = argparse.ArgumentParser()
    ap.add_argument("sbtest")
    ap.add_argument("--all", action="store_true", help="every pair of the samples, not a selection")
    ap.add_argument("--samples", default="/Volumes/Storage/Rocksmith/songs/dlc-current")
    ap.add_argument("--converter", default="/Volumes/Storage/Rocksmith/tools/psarc-to-mac/psarc_to_mac.py")
    ap.add_argument("--plain", default="/Volumes/Storage/Rocksmith/build/seven-mac-work/seven/plain/Ave-Mujica_A-Song-Of-Romance-plain_p.psarc")
    a = ap.parse_args()
    sbtest = os.path.abspath(a.sbtest)
    spec = importlib.util.spec_from_file_location("conv", a.converter)
    conv = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(conv)

    pairs = []
    for root, _, files in os.walk(a.samples):
        for f in sorted(files):
            if f.endswith("_p.psarc") and os.path.exists(os.path.join(root, f[:-8] + "_m.psarc")):
                pairs.append((os.path.join(root, f), os.path.join(root, f[:-8] + "_m.psarc")))
    pairs.sort()
    if not pairs:
        sys.exit("no pairs of x_p.psarc and x_m.psarc under " + a.samples)
    small = min(pairs, key=lambda pm: os.path.getsize(pm[0]))
    if not a.all:
        big = [pm for pm in pairs if os.path.getsize(pm[0]) > 20 << 20][:1]  # a pack of many songs, if there is one
        step = max(1, len(pairs) // 6)
        pairs = sorted(set(pairs[::step][:6] + big + [small]))
    mac_only = []
    for root, _, files in os.walk(a.samples):
        for f in sorted(files):
            if f.endswith("_m.psarc") and not os.path.exists(os.path.join(root, f[:-8] + "_p.psarc")):
                mac_only.append(os.path.join(root, f))
    mac_only = mac_only if a.all else mac_only[::max(1, len(mac_only) // 6)][:6]

    tmp = tempfile.mkdtemp(prefix="songbridge-test-")
    try:
        print("Windows archives in the Mac edition (%d):" % len(pairs))
        for p, m in pairs:
            win_to_mac(p, m, tmp)
        if a.plain and os.path.exists(a.plain):
            win_to_mac(a.plain, None, tmp)
        else:
            print("  (no plain archive to try: %s)" % a.plain)
        print("Mac archives in Learn & Play (%d with the Windows archive to compare, %d without):" % (len(pairs), len(mac_only)))
        for p, m in pairs:
            mac_to_win(m, p, tmp)
        for m in mac_only:
            mac_to_win(m, None, tmp)
        print("Rules:")
        rules("mac-x64", small[0], small[1], tmp)
        rules("win-x86", small[1], small[0], tmp)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("passed: %d checks" % checks)


if __name__ == "__main__":
    main()
