#!/usr/bin/env python3
"""Checks the mod on real archives, without a game.

usage: build.sh --test [--all] [--samples DIR] [--converter psarc_to_mac.py] [--plain X_p.psarc] [--native X_rsxr_p.psarc]...

SBTEST (tests/sbtest.c) starts the mod for one edition and reads through rc_file.c, as the game does. For each
sample this compares the archive the mod serves with what the offline converter makes of the same file (every
entry's content; a song file's after decrypting it with the edition's key), and, where the samples hold the same
song for both platforms, with the other platform's real archive. Then the listing rules: one archive a song, and
files that are not whole archives left alone.

A native package of RS Extended Range (an x_rsxr_p.psarc, a plain archive with the magic PSXR, N-string charts whose
inflated body starts SNXR and manifests with ExtendedRange) is served in both directions as it is but for its
platform's parts: its magic, its plain table of contents and its plain song files kept; its Mac form is made here from
it. The packages tried: each --native that is there (by default two DotCharter exports: a 7-string lead, a 5-string
bass and a stock rhythm; an 8-string lead), and ones made here from the smallest sample with a bass: a 7- and an
8-string lead, a 5- and a 6-string bass, and a 7-string one encrypted (table of contents and song files), with the
sample itself as the stock control.

Without samples (--samples names no folder with pairs) the parts that need them are skipped, and said so.

Nothing is written next to the samples: the folders the mod sees are temporary, with links to them.
"""
import argparse
import collections
import hashlib
import importlib.util
import json
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
CUSTOM_APP = b"248750"  # archives with the base game's AppID (221680), and custom songs, are served with this
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


def magic_of(path):
    with open(path, "rb") as f:
        return f.read(4)


def read(path):
    """conv.read_psarc's content of the archive; a native package (PSXR) is read as the same archive with PSAR"""
    if magic_of(path) != b"PSXR":
        return conv.read_psarc(path)[1]
    d = bytearray(open(path, "rb").read())
    d[:4] = b"PSAR"
    fd, tmp = tempfile.mkstemp(suffix=".psarc")
    try:
        os.write(fd, d)
        os.close(fd)
        return conv.read_psarc(tmp)[1]
    finally:
        os.unlink(tmp)


def toc_flags(path):
    with open(path, "rb") as f:
        return struct.unpack(">I", f.read(32)[28:32])[0]


def strict(path, want_magic=b"PSAR", want_flags=None):
    """The archive read as a strict reader does: every entry inside the file, a block compressed exactly when it is
    shorter than 64 KiB and starts with a zlib header. names -> content, in the table's order. want_flags: the
    header's flags (None: any with 4, an encrypted table of contents)."""
    d = open(path, "rb").read()
    magic, ver, comp, toc_len, esize, n, bsize, flags = struct.unpack(">4sI4sIIIII", d[:32])
    ok(magic == want_magic, "magic %s, as the source's" % want_magic.decode())
    ok(ver == 0x10004 and comp == b"zlib" and esize == 30 and bsize == BLOCK, "header")
    if want_flags is None:
        ok(flags & 4, "table of contents encrypted")
    else:
        ok(flags == want_flags, "flags %d, as the source's (table of contents %s)" % (want_flags, "encrypted" if want_flags & 4 else "plain"))
    toc = d[32:toc_len]
    if flags & 4:
        toc = conv.aes("cfb", conv.TOC_KEY, conv.TOC_IV, toc, True)
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
    native = magic_of(src) == b"PSXR"
    content = strict(out, magic_of(src), toc_flags(src) if native else None)
    lenient = read(out)
    ok(list(lenient.items()) == list(content.items()), "the converter's reader reads the same")
    size = os.path.getsize(out)
    shutil.rmtree(d)
    return content, size


def sng_flags(data):
    return struct.unpack("<I", data[4:8])[0]


def check_sng(name, data, want_payload):
    magic, flags = struct.unpack("<II", data[:8])
    ok(magic == 0x4a and flags == 3, name + ": an encrypted song file")
    ok(data[-56:] == bytes(56), name + ": signature zero")
    ok(sng_plain(name, data) == want_payload, name + ": decrypts to the same song")


def body(name, sng):
    """A song file's inflated body"""
    payload = sng_plain(name, sng)
    raw = zlib.decompress(payload[4:])
    ok(len(raw) == struct.unpack("<I", payload[:4])[0], name + ": its size")
    return raw


def check_song_file(name, served_name, data, v, native):
    """A song file as served: a native package's plain one as it is, any other encrypted with the edition's key"""
    if native and sng_flags(data) == 1:
        ok(v == data, name + ": a native package's plain song file, as it is")
    else:
        check_sng(served_name, v, sng_plain(name, data))
        ok(len(v) == len(data), name + ": length kept")


INSTRUMENT = {0: "guitar", 1: "bass"}


def native_checks(src, got, name_to):
    """For a native package (PSXR): what the native format's reader needs is there as the source has it. Its charts,
    for the log: arrangement and what it is (guitar 7, bass 5, stock)"""
    charts = []
    for name, data in src.items():
        v = got[name_to(name)]
        if is_sng(name):
            want = body(name, data)
            ok(body(name_to(name), v) == want, name + ": the inflated body as the source's" + (" (SNXR)" if want[:4] == b"SNXR" else ""))
            arr = name.rsplit("_", 1)[-1][:-4]
            if want[:4] == b"SNXR":
                ok(want[4] == 1 and want[5] in INSTRUMENT, name + ": an SNXR header (version 1, guitar or bass)")
                charts.append("%s %s %d" % (arr, INSTRUMENT[want[5]], want[6]))
            elif arr != "vocals":
                charts.append(arr + " stock")
        elif name.startswith("manifests/") and b'"ExtendedRange"' in data:
            ok(v == data and b'"Tuning"' in v, name + ": ExtendedRange and Tuning as the source has them")
    ok(any(c.split()[1] != "stock" for c in charts), "a native package with a native chart")
    return ", ".join(sorted(charts))


def mac_form(p, out):
    """The Mac form of the native package p, as rocksmithconvert makes it: names and aggregate graph the Mac's, the
    magic PSXR, the table of contents and plain song files as they are (an encrypted song file: the Mac's key)"""
    mac = {}
    for name, data in read(p).items():
        if name.endswith("aggregategraph.nt"):
            data = conv.to_mac(data.decode()).replace("dx9", "macos").encode()
        elif is_sng(name) and sng_flags(data) == 3:
            data = conv.sng_file(sng_plain(name, data), False)
        mac[conv.to_mac(name)] = data
    write_package(out, mac, b"PSXR", toc_flags(p) & 4)


def write_package(out, content, magic, encrypt):
    conv.write_psarc(out, content, bool(encrypt))
    with open(out, "r+b") as f:
        f.write(magic)


def sng_file_pc(payload, plain):
    """A Windows song file of payload (u32 size + zlib stream): plain, or encrypted with the PC key"""
    if plain:
        return conv.sng_file(payload, True)
    iv = bytes(16)
    return struct.pack("<II", conv.SNG_MAGIC, 3) + iv + conv.aes("ctr", conv.SNG_KEY["pc"], iv.hex(), payload, False) + bytes(conv.SIG_LEN)


def native_body(raw, instrument, strings):
    """An SNXR body for the log and for Song Bridge: the native header and a stock body after it (its records are
    not N wide: what the mod serves is the package's bytes, whatever they say), and the padding the format has"""
    return struct.pack("<4sBBBB", b"SNXR", 1, instrument, strings, 0) + raw + bytes(256)


def synth_native(base, out, arrangement, instrument, strings, encrypted=False):
    """A native package made of the stock Windows archive base: the chart of `arrangement` an SNXR one for
    instrument/strings, its manifests with ExtendedRange and an N-string Tuning; the other charts stock. Plain (the
    table of contents, flags 1 song files) unless encrypted."""
    src, content, ids = read(base), {}, set()
    for name, data in src.items():
        if is_sng(name):
            payload = sng_plain(name, data)
            if name.endswith("_%s.sng" % arrangement):
                raw = zlib.decompress(payload[4:])
                b = native_body(raw, instrument, strings)
                payload = struct.pack("<I", len(b)) + zlib.compress(b, 9)
            data = sng_file_pc(payload, not encrypted)
        content[name] = data
    ok(any(is_sng(k) and k.endswith("_%s.sng" % arrangement) for k in content), "%s: a %s chart" % (base, arrangement))
    tuning = {"string%d" % i: 0 for i in range(strings)}
    for name in [k for k in content if k.startswith("manifests/") and k.endswith("_%s.json" % arrangement)]:
        j = json.loads(content[name])
        for key, e in j["Entries"].items():
            e["Attributes"].update(ExtendedRange={"Version": 1}, Tuning=tuning)
            ids.add(key)
        content[name] = json.dumps(j, indent=2).encode()
    for name in [k for k in content if k.startswith("manifests/") and k.endswith(".hsan")]:
        j = json.loads(content[name])
        for key in ids & set(j["Entries"]):
            j["Entries"][key]["Attributes"].update(ExtendedRange={"Version": 1}, Tuning=tuning)
        content[name] = json.dumps(j, indent=2).encode()
    write_package(out, content, b"PSXR", encrypted)


def win_to_mac(p, twin, tmp):
    got, size = served("mac-x64", p, tmp)
    src, native = read(p), magic_of(p) == b"PSXR"
    ok(list(got) == [conv.to_mac(k) for k in src], "names")
    nsng = 0
    for name, data in src.items():
        v = got[conv.to_mac(name)]
        if is_sng(name):
            check_song_file(name, conv.to_mac(name), data, v, native)
            nsng += 1
        elif name.endswith("aggregategraph.nt"):
            ok(v == conv.to_mac(data.decode()).replace("dx9", "macos").encode(), "aggregate graph as the converter's")
        elif name == "appid.appid" and ("toolkit.version" in src or data.strip() == b"221680"):
            ok(v == CUSTOM_APP, "appid.appid: served with the free DLC's AppID")
        else:
            ok(v == data, name + ": unchanged")
    note, differ = "", []
    if twin:
        real = read(twin)
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
    if native:
        note += " (%s)" % native_checks(src, got, conv.to_mac)
    print("  mac-x64  %-34s %9d bytes, %2d entries, %d song files%s" % (os.path.basename(p), size, len(got), nsng, note))


def mac_to_win(m, twin, tmp):
    got, size = served("win-x86", m, tmp)
    src, native = read(m), magic_of(m) == b"PSXR"
    ok(list(got) == [to_win(k) for k in src], "names")
    nsng = 0
    for name, data in src.items():
        w = to_win(name)
        v = got[w]
        ok("macos" not in w and "audio/mac" not in w, w + ": a Windows name")
        if is_sng(name):
            check_song_file(name, w, data, v, native)
            nsng += 1
        elif name.endswith("aggregategraph.nt"):
            ok(b"macos" not in v and b"audio/mac" not in v, "aggregate graph: nothing of the Mac's")
            ok(conv.to_mac(v.decode()).replace("dx9", "macos").encode() != v, "aggregate graph: Windows tags")
            # back through the converter: the Mac's graph but its song files' macos tags
            back = lines(conv.to_mac(v.decode()).replace("dx9", "macos").encode())
            diff = lines(data) - back
            ok(not (back - lines(data)) and set(diff) <= {MAC_SNG_TAG}, "aggregate graph: converts back")
        elif name == "appid.appid" and ("toolkit.version" in src or data.strip() == b"221680"):
            ok(v == CUSTOM_APP, "appid.appid: served with the free DLC's AppID")
        else:
            ok(v == data, name + ": unchanged")
    note, differ = "", []
    if twin:
        real = read(twin)
        ok(list(real) == list(got), "names as the real Windows archive's")
        for name, data in real.items():
            if is_sng(name):
                ok(sng_plain(name, data) == sng_plain(name, got[name]), name + ": the real Windows archive's song")
                if native and sng_flags(data) == 1:
                    ok(data == got[name], name + ": the Windows package's own bytes")
            elif name.endswith("aggregategraph.nt"):
                ok(lines(data) == lines(got[name]), "aggregate graph: the real Windows archive's lines")
            elif data != got[name]:
                # the platforms' real archives differ in this file themselves: not the mod's doing (it serves the
                # source's bytes, checked above)
                ok(not name.startswith(("manifests/", "gamexblocks/", "songs/")), name + ": the real Windows archive's")
                differ.append(name)
        note = ", matches " + os.path.basename(twin) + (" but its own " + ", ".join(differ) if differ else "")
    if native:
        note += " (%s)" % native_checks(src, got, to_win)
    print("  win-x86  %-34s %9d bytes, %2d entries, %d song files%s" % (os.path.basename(m), size, len(got), nsng, note))


def rules(target, foreign, native, tmp, tag=""):
    """The listing: foreign is an archive of the other platform, native the same song for target's"""
    fs, ns = SFX[OTHER[target]], SFX[target]
    n = [0]

    def folder(*files):
        n[0] += 1
        dlc = os.path.join(tmp, "rules%s-%s-%d" % (tag, target, n[0]), "dlc")
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
    d = os.path.join(tmp, "elsewhere%s-" % tag + target)
    os.mkdir(d)
    os.symlink(foreign, d + "/a" + fs)
    ok(listing(target, d) == ["a" + fs] and stat(d + "/a" + ns) == "none", "outside dlc: untouched")
    # an edition's own archives alone: nothing changes
    dlc = folder(("a" + ns, native), ("sub/b" + ns, native))
    ok(listing(target, dlc) == ["a" + ns, "sub"] and stat(dlc + "/a" + ns) == "real", "own archives alone: untouched")
    print("  %s  listing rules, files that are not archives%s" % (target, tag and " (%s)" % tag))


SAMPLES = "/Volumes/Storage/Rocksmith/songs/backup-dlc"
# DotCharter exports: a 7-string lead, a 5-string bass, a stock 6-string rhythm and vocals; an 8-string lead. The
# first of each group that is there is tried.
NATIVE = [[os.path.expanduser("~/Downloads/Nickelback_Burn-It-To-The-Ground_Dell_v2_3_rsxr_p.psarc"),
           "/Volumes/Storage/Rocksmith/build/seven-native/Nickelback_Burn-It-To-The-Ground_Dell_v2_3_rsxr_p.psarc"],
          [os.path.expanduser("~/Downloads/Fear-Factory_Soul-Hacker_Dell_v0_2_rsxr_p.psarc")]]
# made of a sample: name, the arrangement made native, instrument, strings, encrypted
SYNTH = [("guitar7", "lead", 0, 7, False), ("guitar8", "lead", 0, 8, False), ("bass5", "bass", 1, 5, False),
         ("bass6", "bass", 1, 6, False), ("guitar7-encrypted", "lead", 0, 7, True)]


def synth_base(pairs):
    """The smallest pair whose Windows archive has a lead and a bass chart, or None"""
    for p, m in sorted(pairs, key=lambda pm: os.path.getsize(pm[0]))[:20]:
        names = list(read(p))
        if any(is_sng(k) and k.endswith("_lead.sng") for k in names) and any(is_sng(k) and k.endswith("_bass.sng") for k in names):
            return p, m
    return None


def native_both_ways(p, tmp):
    """The native package p in the Mac edition, and its Mac form in Learn & Play; the Mac form's path"""
    ok(magic_of(p) == b"PSXR" and p.endswith("_rsxr_p.psarc"), "%s: an x_rsxr_p.psarc with the magic PSXR" % p)
    nm = os.path.join(tmp, os.path.basename(p)[:-len("_p.psarc")] + "_m.psarc")
    mac_form(p, nm)
    win_to_mac(p, None, tmp)
    mac_to_win(nm, p, tmp)
    return nm


def main():
    global conv, sbtest
    ap = argparse.ArgumentParser()
    ap.add_argument("sbtest")
    ap.add_argument("--all", action="store_true", help="every pair of the samples, not a selection")
    ap.add_argument("--samples", default=SAMPLES, help="a folder with x_p.psarc and x_m.psarc of the same songs")
    ap.add_argument("--converter", default="/Volumes/Storage/Rocksmith/tools/psarc-to-mac/psarc_to_mac.py")
    ap.add_argument("--plain", default="/Volumes/Storage/Rocksmith/build/seven-mac-work/seven/plain/Ave-Mujica_A-Song-Of-Romance-plain_p.psarc")
    ap.add_argument("--native", action="append", help="an RS Extended Range native package (PSXR); again for more")
    a = ap.parse_args()
    sbtest = os.path.abspath(a.sbtest)
    if not os.path.exists(a.converter):
        sys.exit("no offline converter to compare with: %s (--converter FILE)" % a.converter)
    spec = importlib.util.spec_from_file_location("conv", a.converter)
    conv = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(conv)

    pairs = []
    for root, _, files in os.walk(a.samples):
        for f in sorted(files):
            if f.endswith("_p.psarc") and os.path.exists(os.path.join(root, f[:-8] + "_m.psarc")):
                pairs.append((os.path.join(root, f), os.path.join(root, f[:-8] + "_m.psarc")))
    pairs.sort()
    every = pairs
    if pairs:
        small = min(pairs, key=lambda pm: os.path.getsize(pm[0]))
        if not a.all:
            big = [pm for pm in pairs if os.path.getsize(pm[0]) > 20 << 20][:1]  # a pack of many songs, if there is one
            step = max(1, len(pairs) // 6)
            pairs = sorted(set(pairs[::step][:6] + big + [small]))
    else:
        print("(no pairs of x_p.psarc and x_m.psarc under %s (--samples DIR): the samples, the listing rules and the\n"
              " native packages made of a sample are skipped)" % a.samples)
    mac_only = []
    for root, _, files in os.walk(a.samples):
        for f in sorted(files):
            if f.endswith("_m.psarc") and not os.path.exists(os.path.join(root, f[:-8] + "_p.psarc")):
                mac_only.append(os.path.join(root, f))
    mac_only = mac_only if a.all else mac_only[::max(1, len(mac_only) // 6)][:6]
    if a.native:
        natives = a.native
        for p in natives:
            ok(os.path.exists(p), "--native %s: there" % p)
    else:
        natives = [next((p for p in group if os.path.exists(p)), None) for group in NATIVE]
        for group, p in zip(NATIVE, natives):
            if not p:
                print("(no native package to try: %s)" % " or ".join(group))
        natives = [p for p in natives if p]

    tmp = tempfile.mkdtemp(prefix="songbridge-test-")
    try:
        if pairs:
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
        print("RS Extended Range native packages, both forms:")
        for p in natives:
            nm = native_both_ways(p, tmp)
            if p == natives[0]:
                rules("mac-x64", p, nm, tmp, "native")
                rules("win-x86", nm, p, tmp, "native")
        base = synth_base(every) if every else None
        if base:
            print("  made of %s, and it as the stock control:" % os.path.basename(base[0]))
            win_to_mac(base[0], base[1], tmp)
            mac_to_win(base[1], base[0], tmp)
            d = os.path.join(tmp, "synth")
            os.mkdir(d)
            for name, arrangement, instrument, strings, encrypted in SYNTH:
                p = os.path.join(d, "synth-%s_rsxr_p.psarc" % name)
                synth_native(base[0], p, arrangement, instrument, strings, encrypted)
                nm = native_both_ways(p, tmp)
                if name == "guitar7":
                    rules("mac-x64", p, nm, tmp, "native, made")
                    rules("win-x86", nm, p, tmp, "native, made")
        elif every:
            print("  (no sample with a lead and a bass chart to make native packages of)")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("passed: %d checks" % checks)


if __name__ == "__main__":
    main()
