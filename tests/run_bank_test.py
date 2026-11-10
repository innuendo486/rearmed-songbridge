#!/usr/bin/env python3
"""The sound bank builder's and the preview cutter's test:
tests/run_bank_test.py BANKTEST --tools DIR [--samples DIR]... [--banks DIR]... [--codebooks FILE]

1. Real banks. Every song_*.bnk of the archives in each --samples folder (read with rs-rearmed's reader in DIR, as
   run_varc_test.py does) and every loose one under each --banks folder (with its <id>.wem next to it, or the start
   of it) is read by the parser here, which accounts for every byte, and made again by banktest from the fields
   read: the same bytes must come out.
2. A bank for made-up keys, with the ids banktest makes: read by the same parser; its ids, sizes and prefetch checked.
3. Previews. Ogg Vorbis files made with sox are repacked (src/wem.c) and cut; the cut's header and packets are
   checked as tools/proto/wwv.py reads them, and its decode (vgmstream-cli) against the same stretch of the whole
   file's decode. With --samples, a .wem of each archive is cut the same way. Needs sox, vgmstream-cli and the game's
   codebook library (default: the one rearmed-mods' stems helper writes); without the library this part is skipped.
4. Damaged .wem files and odd bank inputs: none may crash (the sanitizers are on in banktest)."""
import argparse, array, collections, os, random, shutil, struct, subprocess, sys, tempfile, wave

checks = 0
PREFETCH = 51200
MIXER = bytes.fromhex("4474c726000029fcf09b0000000000000000000101" + "00" * 25 + "01000000")


def ok(cond, what):
    global checks
    checks += 1
    if not cond:
        sys.exit("FAIL: " + what)


def fnv(s):
    h = 2166136261
    for c in s.lower().encode("latin1"):
        h = (h * 16777619 & 0xffffffff) ^ c
    return h


def id31(s):
    h = fnv(s)
    return (h >> 31) ^ (h & 0x7fffffff) or 1


class Bytes:
    """A reader that must be read to its end"""
    def __init__(s, d, what): s.d, s.p, s.what = d, 0, what
    def take(s, n):
        ok(s.p + n <= len(s.d), s.what + ": ends early")
        s.p += n
        return s.d[s.p - n:s.p]
    def u8(s): return s.take(1)[0]
    def u16(s): return struct.unpack("<H", s.take(2))[0]
    def u32(s): return struct.unpack("<I", s.take(4))[0]
    def const(s, b, what): ok(s.take(len(b)) == b, "%s: %s is not %s" % (s.what, what, b.hex()))
    def end(s): ok(s.p == len(s.d), s.what + ": bytes left over")


def parse_bank(d, tag):
    """Every field of a song's bank that is not the same in all of them; fails on anything unknown"""
    b = Bytes(d, tag)
    def section(name):
        ok(b.take(4) == name, "%s: no %s where it should be" % (tag, name.decode()))
        return Bytes(b.take(b.u32()), tag + " " + name.decode())
    f = {}
    s = section(b"BKHD")
    s.const(struct.pack("<I", 0x5b), "version"); f["bank"] = s.u32(); s.const(bytes(20), "the header's rest"); s.end()
    s = section(b"DIDX")
    f["wem"] = s.u32(); s.const(bytes(4), "the prefetch's place"); size = s.u32(); s.end()
    f["data"] = section(b"DATA").d
    ok(len(f["data"]) == size, tag + ": DIDX's size is DATA's")
    ok(b.p - len(f["data"]) == 64, tag + ": DATA's bytes start at 64")
    h = section(b"HIRC")
    ok(h.u32() == 4, tag + ": four objects")
    def obj(t):
        ok(h.u8() == t, "%s: object type %d" % (tag, t))
        return Bytes(h.take(h.u32()), "%s object %d" % (tag, t))
    s = obj(2)
    f["sound"] = s.u32(); s.const(struct.pack("<II", 0x00040001, 2), "codec and stream type")
    ok(s.u32() == f["wem"] and s.u32() == f["wem"], tag + ": source and file id are DIDX's")
    f["media_off"], f["media_size"] = s.u32(), s.u32()
    s.const(b"\0\1\0", "language and effects")
    bus = s.u32()
    ok(bus in (0, 0xf908c29a), tag + ": the sound's bus")
    f["preview"] = int(bus != 0)
    s.const(MIXER[:4] + bytes(3), "parent and priority")
    n = s.u8()
    ok(n in (3, 4), tag + ": 3 or 4 properties")
    s.const(b"\x00\x2e\x2f\x36"[:n], "the properties' ids")
    f["volume"] = s.u32(); s.const(struct.pack("<II", 1, 3), "properties 0x2e and 0x2f")
    f["has36"] = int(n == 4); f["p36"] = s.u32() if n == 4 else 0
    s.const(bytes(6), "ranges, positioning, sends")
    f["elapsed"], kill, virt, limit, glob, below, own, vv = s.u8(), s.u8(), s.u8(), s.u16(), s.u8(), s.u8(), s.u8(), s.u8()
    ok((kill, limit, own) == ((1, 1, 1) if f["preview"] else (0, 0, 0)), tag + ": a preview plays one at a time, a song without a limit")
    ok((virt, glob, below, vv) == (0, 0, 0, 0) and f["elapsed"] in (0, 1) and (f["elapsed"] or not f["preview"]), tag + ": the voice flags")
    s.const(bytes(10), "states and game parameters"); s.end()
    s = obj(7); s.const(MIXER, "the actor-mixer"); ok(s.u32() == f["sound"], tag + ": the mixer's child is the sound"); s.end()
    s = obj(3)
    f["action"] = s.u32(); s.const(b"\x03\x04", "play"); ok(s.u32() == f["sound"], tag + ": the action plays the sound")
    s.const(b"\0\0\0\4", "the action's rest"); ok(s.u32() == f["bank"], tag + ": the action names the bank"); s.end()
    s = obj(4)
    f["event"] = s.u32(); ok(s.u32() == 1 and s.u32() == f["action"], tag + ": the event has the action"); s.end()
    h.end()
    s = section(b"STID")
    s.const(struct.pack("<II", 1, 1), "one name"); ok(s.u32() == f["bank"], tag + ": the name is the bank's")
    f["name"] = s.take(s.u8()).decode("latin1"); s.end()
    b.end()
    return f


def remake(banktest, f, tmp):
    """The bank banktest writes from the fields parse_bank read"""
    pre, out = os.path.join(tmp, "pre.bin"), os.path.join(tmp, "remade.bnk")
    open(pre, "wb").write(f["data"])
    r = subprocess.run([banktest, "write", pre, out, f["name"], str(f["preview"])] +
                       ["%x" % f[k] for k in ("bank", "wem", "sound", "action", "event", "volume", "media_off", "media_size")] +
                       [str(f["elapsed"]), str(f["has36"]), "%x" % f["p36"]], capture_output=True, text=True)
    ok(r.returncode == 0, "banktest write: %s%s" % (r.stdout, r.stderr))
    return open(out, "rb").read()


class Stats:
    def __init__(s): s.n = 0; s.c = collections.Counter()


def real_bank(banktest, name, d, wem_head, tmp, st):
    """One real bank: parsed, made again, and what holds for all of them"""
    base = os.path.basename(name)
    f = parse_bank(d, base)
    again = remake(banktest, f, tmp)
    ok(again == d, "%s: made again from its fields it differs (first at byte %d)" %
       (name, next((i for i, (x, y) in enumerate(zip(again, d)) if x != y), min(len(again), len(d)))))
    ok(f["name"].lower() == base[:-4].lower() and f["name"].lower().startswith("song_"), base + ": the bank's name is the file's")
    ok(f["preview"] == int(f["name"].lower().endswith("_preview")), base + ": the previews' bus in the preview's bank only")
    key = f["name"][5:-8] if f["preview"] else f["name"][5:]
    ok(f["event"] == fnv("play_" + key + ("_preview" if f["preview"] else "")), base + ": the event is FNV-1 of its name")
    ok(all(0 < f[k] < 1 << 31 for k in ("wem", "sound", "action")), base + ": .wem, sound and action ids below 2^31")
    if wem_head is not None:
        ok(wem_head[:len(f["data"])] == f["data"], base + ": DATA is the .wem's start")
    st.n += 1
    st.c["bank id is FNV-1 of the name" if f["bank"] == fnv(f["name"]) else "bank id is something else"] += 1
    st.c["the sound says where the prefetch is" if (f["media_off"], f["media_size"]) == (0, len(f["data"])) else "junk for the prefetch's place"] += 1
    st.c["a fourth property" if f["has36"] else "three properties"] += 1
    if not f["preview"]:
        st.c["main: virtual voice %d" % f["elapsed"]] += 1
    st.c["prefetch %s" % (len(f["data"]) if len(f["data"]) in (51200, 72000) else "of another size")] += 1


def samples(path):
    w = wave.open(path)
    return w.getnchannels(), w.getnframes(), array.array("h", w.readframes(w.getnframes()))


def check_cut(wwv, src, cut, at, start, seconds, tag, decoded, loud=True):
    """The cut's header and packets against the source's, and its decode against decoded (the source's); loud: the
    stretch is known not to be silence. Returns (frames, the source's frame it starts at, the worst difference)."""
    h, seek, setup, pk, data = wwv.read_wem(cut)
    H, _, SETUP, PK, _ = src
    modes = wwv.parse_ww_setup(setup, h["ch"])["modes"]
    mb, bs = wwv.ilog(len(modes) - 1), [1 << h["bs"][0], 1 << h["bs"][1]]
    size = lambda p: bs[modes[wwv.R(p).get(mb)]]
    def cumulative(packets):  # F(i): frames decoded through packet i
        out, prev = [], None
        for _, p in packets:
            out.append(0 if prev is None else out[-1] + (prev + size(p)) // 4)
            prev = size(p)
        return out
    F, G = cumulative(PK), cumulative(pk)
    ok(setup == SETUP and h["hash"] == H["hash"] and h["bs"] == H["bs"] and (h["ch"], h["rate"], h["mask"]) == (H["ch"], H["rate"], H["mask"]),
       tag + ": the setup, its key, the block sizes and the format are the source's")
    ok((h["alloc"], h["alloc64"]) == (H["alloc"], H["alloc64"]), tag + ": the decoder's room is the source's")
    ok(pk and pk[-1][0] + 2 + len(pk[-1][1]) == h["data_size"], tag + ": the packets end where the data ends")
    ok(os.path.getsize(cut) == 94 + h["data_size"] and h["chunks"] == ["fmt ", "data"], tag + ": the file is header and data")
    riff = struct.unpack("<I", open(cut, "rb").read(8)[4:])[0]
    ok(riff == os.path.getsize(cut) - 8, tag + ": the RIFF size")
    ok(h["data_off"] == h["seek_size"] + 2 + len(setup) and h["loop_start"] == 2 + len(setup) and h["loop_end"] == h["data_size"] - h["seek_size"],
       tag + ": the offsets in the header")
    ok(h["max_packet"] == max(len(p) for _, p in pk), tag + ": the largest packet")
    ok(h["loop_begin_extra"] == 0 and h["loop_end_extra"] == h["last_extra"], tag + ": the extra frames, twice")
    ok(G[-1] - h["last_extra"] == h["frames"], "%s: %d frames by block sizes less %d extra, the header says %d" % (tag, G[-1], h["last_extra"], h["frames"]))
    ok(h["avg"] == (h["data_size"] * h["rate"] // h["frames"] if h["frames"] else 0), tag + ": bytes a second")
    # the packets are a run of the source's, from the packet banktest named by its frame
    first = next((i for i in range(len(PK)) if F[i] == at and PK[i][1] == pk[0][1]), None)
    ok(first is not None and [p for _, p in PK[first:first + len(pk)]] == [p for _, p in pk], tag + ": a run of the source's packets from frame %d" % at)
    last = first + len(pk) - 1
    # where it starts and how long it is
    played = min(H["frames"], F[-1])
    want = min(played, int((seconds if seconds > 0 else 30.0) * h["rate"]))
    frm = min(played, int(start * h["rate"])) if start >= 0 else played // 3
    frm = min(frm, played - want)
    ok(F[first] <= frm and (first + 1 == len(PK) or F[first + 1] > frm), "%s: starts at the last packet with F <= %d (F = %d)" % (tag, frm, F[first]))
    ok(G[-1] >= want and (len(pk) < 2 or G[-2] < want), "%s: ends with the first packet that reaches %d frames (%d)" % (tag, want, G[-1]))
    if last == len(PK) - 1:
        ok(h["last_extra"] == min(F[-1] - played, G[-1]), tag + ": reaching the end, the source's extra frames")
    else:
        ok(h["last_extra"] == 0, tag + ": no extra frames inside the stream")
    ok(h["frames"] >= want and h["frames"] - want < bs[1] and frm - F[first] < bs[1], tag + ": start and length within a long block of what was asked")
    # the seek table: each entry a packet's start and the frames decoded through that packet; entries often enough
    offs = {o: i for i, (o, _) in enumerate(pk)}
    af = ao = 0
    for df, do in seek:
        af += df; ao += do
        i = offs.get(ao + h["seek_size"])
        ok(i is not None and i > 0 and G[i] == af, "%s: a seek entry at a packet's start with its frames (%d, %d)" % (tag, af, ao))
    ok(G[-1] - af < 16384 + bs[1] and h["data_size"] - h["seek_size"] - ao < 0x10000 + 0xc000, tag + ": seek entries to the end")
    subprocess.run(["vgmstream-cli", "-o", cut + ".wav", cut], check=True, capture_output=True)
    ch, n, x = samples(cut + ".wav")
    ok(n == h["frames"], "%s: decodes to %d frames, the header says %d" % (tag, n, h["frames"]))
    ch2, N, y = decoded
    ok(ch == ch2 and at + n <= N, "%s: the stretch %d..%d is inside the source's %d frames" % (tag, at, at + n, N))
    y = y[at * ch:(at + n) * ch]
    worst = max((abs(p - q) for p, q in zip(x, y)), default=0)
    ok(worst <= 2, "%s: a sample %d from the source's at the same place" % (tag, worst))
    ok(not loud or max(map(abs, y)) > 1000, tag + ": the stretch is not silence")
    return h["frames"], at, worst


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("banktest")
    ap.add_argument("--tools", required=True)
    ap.add_argument("--samples", action="append", default=[])
    ap.add_argument("--banks", action="append", default=[])
    ap.add_argument("--codebooks", default=os.path.expanduser("~/Library/Caches/RS Re-ARMed/stems/codebooks.bin"))
    a = ap.parse_args()
    here = os.path.dirname(os.path.abspath(__file__))
    sys.path.insert(0, here)
    sys.path.insert(0, os.path.join(here, "..", "tools", "proto"))
    sys.dont_write_bytecode = True  # nothing left next to the sources
    import run_varc_test as rv, wwv
    bt = a.banktest
    run = lambda *args: subprocess.run([bt] + [str(x) for x in args], capture_output=True, text=True)
    rng = random.Random(11)
    real_wems = []  # (tag, path): one .wem of each sample archive, for the cutter

    with tempfile.TemporaryDirectory() as tmp:
        # 1. real banks
        for folder in a.samples:
            px = rv.reader(a.tools, os.path.join(here, "..", "src", "keys.h"))
            st = Stats()
            for name in sorted(os.listdir(folder)):
                if not name.endswith(".psarc"):
                    continue
                ents = {n: d for n, d, _ in rv.read_all(px, os.path.join(folder, name))}
                kept = False
                for n, d in sorted(ents.items()):
                    if not n.endswith(".bnk"):
                        continue
                    wid = struct.unpack_from("<I", d, 44)[0]
                    wem = ents.get(os.path.dirname(n) + "/%d.wem" % wid)
                    ok(wem is not None, "%s: %s: the .wem its bank names is in the archive" % (name, n))
                    real_bank(bt, name + ":" + n, d, wem, tmp, st)
                    if not kept and not n.endswith("_preview.bnk") and len(wem) < 40 << 20:
                        p = os.path.join(tmp, "real%d.wem" % len(real_wems))
                        open(p, "wb").write(wem)
                        real_wems.append((name, p))
                        kept = True
            print("%d banks of the archives in %s read and made again, byte for byte: %s" % (st.n, folder, dict(st.c)))
        for folder in a.banks:
            st = Stats()
            for dp, _, files in sorted(os.walk(folder)):
                for n in sorted(files):
                    if n.endswith(".bnk"):
                        d = open(os.path.join(dp, n), "rb").read()
                        wp = os.path.join(dp, "%d.wem" % struct.unpack_from("<I", d, 44)[0])
                        real_bank(bt, os.path.join(dp, n), d, open(wp, "rb").read(200000) if os.path.exists(wp) else None, tmp, st)
            print("%d loose banks under %s read and made again, byte for byte: %s" % (st.n, folder, dict(st.c)))

        # 2. made-up keys
        seen = {}
        for key, size in (("zzfeedpaktest", 300000), ("ZZFeedPakTest", 300000), ("a", 1), ("k" * 200, PREFETCH), ("sb_0001", PREFETCH + 1), ("sb_0002", 777)):
            wem = os.path.join(tmp, "made.wem")
            body = bytes(rng.getrandbits(8) for _ in range(size))
            open(wem, "wb").write(body)
            for preview in (0, 1):
                tag = "%s%s" % (key[:20], " preview" if preview else "")
                out = os.path.join(tmp, "made.bnk")
                r = run("build", key, preview, wem, out)
                ok(r.returncode == 0, "%s: banktest build: %s%s" % (tag, r.stdout, r.stderr))
                said = dict(w.split("=") for w in r.stdout.split() if "=" in w)
                d = open(out, "rb").read()
                f = parse_bank(d, tag)
                ok(len(d) == int(said["size"]) == int(said["foretold"]), "%s: %d bytes, %s foretold" % (tag, len(d), said["foretold"]))
                ok(len(d) == 284 + min(size, PREFETCH) + len(f["name"]), tag + ": the size by the layout")
                ok(f["data"] == body[:PREFETCH] and int(said["prefetch"]) == len(f["data"]), tag + ": DATA is the .wem's first min(size, %d) bytes" % PREFETCH)
                ok((f["media_off"], f["media_size"]) == (0, len(f["data"])), tag + ": the sound says where the prefetch is")
                suffix = "_preview" if preview else ""
                ok(f["name"] == "Song_" + key + ("_Preview" if preview else "") and f["preview"] == preview, tag + ": the bank's name")
                ok(f["event"] == fnv("play_" + key + suffix), tag + ": the event is FNV-1 of play_<key>" + suffix)
                ok(f["bank"] == fnv("song_" + key + suffix), tag + ": the bank id is FNV-1 of the bank's name")
                ok(f["wem"] == id31("song_" + key + suffix + ".wem") and f["sound"] == id31("song_" + key + suffix + ".sound")
                   and f["action"] == id31("song_" + key + suffix + ".play"), tag + ": .wem, sound and action ids by the scheme")
                ok(all(0 < f[k] < 1 << 31 for k in ("wem", "sound", "action")), tag + ": those ids are 1 to 2^31-1")
                ok(f["volume"] == struct.unpack("<I", struct.pack("<f", -7.0))[0] and f["elapsed"] == 1 and not f["has36"], tag + ": volume and flags")
                ok(remake(bt, f, tmp) == d, tag + ": made again from its fields it is the same")
                ok(run("build", key, preview, wem, out + "2").returncode == 0 and open(out + "2", "rb").read() == d, tag + ": the same on a second run")
                seen[(key.lower(), preview)] = f
        ok(seen[("zzfeedpaktest", 0)]["wem"] != seen[("zzfeedpaktest", 1)]["wem"], "main and preview .wem ids differ")
        ids = {}
        r = run("ids", *["sb_%04d" % i for i in range(3000)])  # a fixed list: no luck involved
        ok(r.returncode == 0 and len(r.stdout.splitlines()) == 6000, "banktest ids")
        for line in r.stdout.splitlines():
            for w in line.split()[1:]:
                k, v = w.split("=")
                ok(int(v, 16) != 0, "an id of 0")
                ids.setdefault(k, []).append(int(v, 16))
        for k, v in ids.items():
            ok(len(set(v)) == len(v), "two of 3000 songs share a %s id" % k)
        ok(len(set(ids["wem"]) | set(ids["sound"]) | set(ids["action"])) == 18000, ".wem, sound and action ids of 3000 songs are all different")
        for bad in ("", "k" * 201):
            r = run("build", bad, 0, wem, os.path.join(tmp, "no.bnk"))
            ok(r.returncode == 3 and "foretold: 0" in r.stdout, "a key of %d characters is refused, its size 0" % len(bad))
        open(os.path.join(tmp, "empty.wem"), "wb").close()
        ok(run("build", "key", 0, os.path.join(tmp, "empty.wem"), os.path.join(tmp, "no.bnk")).returncode == 3, "no bank without a prefetch")
        print("made-up banks: main and preview for 6 keys, sizes foretold, ids by the scheme; 18000 ids of 3000 keys all different")

        # 3. previews
        if not os.path.exists(a.codebooks):
            print("skipped: the previews (no codebook library at %s)" % a.codebooks)
            print("ok, %d checks" % checks)
            return
        for tool in ("sox", "vgmstream-cli"):
            if not shutil.which(tool):
                sys.exit("needs " + tool)
        wav = os.path.join(tmp, "in.wav")
        subprocess.run(["sox", "-n", "-r", "48000", "-c", "2", "-b", "16", wav, "synth", "90", "sine", "80-9000", "pinknoise",
                        "tremolo", "3", "90", "gain", "-3"], check=True, capture_output=True)
        made = [("q5-48k", ["-C", "5"], []), ("q2-48k", ["-C", "2"], []), ("q8-48k", ["-C", "8"], []),
                ("q5-44k", ["-r", "44100", "-C", "5"], []), ("q5-mono", ["-c", "1", "-C", "5"], []),
                ("q3-44k-mono", ["-r", "44100", "-c", "1", "-C", "3"], []),
                ("q5-5s", ["-C", "5"], ["trim", "20", "5"]), ("q5-31s", ["-C", "5"], ["trim", "0", "31"])]
        cuts = [(-1, 0), (0, 0), (10, 0), (59.99, 0), (1000, 0), (40.3, 12.5), (89.9, 3), (5, 0.01)]
        worst_all, rows = 0, []
        fuzz_src = None
        for tag, opts, fx in made:
            ogg, wem = os.path.join(tmp, tag + ".ogg"), os.path.join(tmp, tag + ".wem")
            subprocess.run(["sox", wav] + opts + [ogg] + fx, check=True, capture_output=True)
            r = run("wem", a.codebooks, ogg, wem)
            ok(r.returncode == 0, "%s: repacked: %s%s" % (tag, r.stdout, r.stderr))
            fuzz_src = fuzz_src or wem
            src = wwv.read_wem(wem)
            subprocess.run(["vgmstream-cli", "-o", wem + ".wav", wem], check=True, capture_output=True)
            decoded = samples(wem + ".wav")
            ok(decoded[1] == src[0]["frames"], tag + ": the whole file decodes to its length")
            for start, seconds in cuts:
                t = "%s from %g for %g" % (tag, start, seconds)
                cut = os.path.join(tmp, "cut.wem")
                r = run("cut", wem, cut, start, seconds)
                ok(r.returncode == 0, "%s: banktest cut: %s%s" % (t, r.stdout, r.stderr))
                at = int(r.stdout.split("at=")[1])
                frames, at, worst = check_cut(wwv, src, cut, at, start, seconds, t, decoded)
                worst_all = max(worst_all, worst)
                rate = src[0]["rate"]
                if (start, seconds) in ((-1, 0), (1000, 0)):
                    rows.append("%s from %g: %.3f s from %.3f s of %.1f s" % (tag, start, frames / rate, at / rate, src[0]["frames"] / rate))
                if seconds == 0:
                    total = src[0]["frames"]
                    ok(frames == total if total <= 30 * rate else 30 * rate <= frames < 30 * rate + 2048, t + ": 30 s, or the whole of a shorter file")
                    if start == 1000 and total > 30 * rate:
                        ok(0 <= total - (at + frames) < 2048, t + ": a start past the end gives the last 30 s (to within a block)")
                    # the preview's bank: its prefetch holds the header, the seek table and the setup packet
                    h = wwv.read_wem(cut)[0]
                    bnk = os.path.join(tmp, "cut.bnk")
                    r = run("build", "zzcut", 1, cut, bnk)
                    ok(r.returncode == 0, t + ": the preview's bank")
                    f = parse_bank(open(bnk, "rb").read(), t + " bank")
                    ok(f["data"] == open(cut, "rb").read()[:PREFETCH] and len(f["data"]) >= 94 + h["data_off"], t + ": the bank's prefetch covers the setup")
        print("previews: %d cuts of %d files, each decoded and compared with the whole file's decode at the frame the cutter named: "
              "worst difference %d of 32768, from the first sample on" % (len(made) * len(cuts), len(made), worst_all))
        for row in rows:
            print("  " + row)
        # the game's own kind of file cuts too (its seek table and setup are another writer's)
        for name, wem in real_wems:
            src = wwv.read_wem(wem)
            subprocess.run(["vgmstream-cli", "-o", wem + ".wav", wem], check=True, capture_output=True)
            decoded = samples(wem + ".wav")
            for start, seconds in ((-1, 0), (1e6, 0), (0, 5)):
                cut = os.path.join(tmp, "cut.wem")
                r = run("cut", wem, cut, start, seconds)
                ok(r.returncode == 0, "%s: banktest cut: %s%s" % (name, r.stdout, r.stderr))
                frames, at, worst = check_cut(wwv, src, cut, int(r.stdout.split("at=")[1]), start, seconds, "%s from %g" % (name, start), decoded,
                                                  loud=start < 0)
                worst_all = max(worst_all, worst)
            print("  %s: its .wem (%.1f s) cut three ways, worst difference so far %d" % (name, src[0]["frames"] / src[0]["rate"], worst_all))

        # 4. damaged input
        base = open(fuzz_src, "rb").read()
        h = wwv.read_wem(fuzz_src)[0]
        first = 94 + h["data_off"]
        size1 = 2 + struct.unpack_from("<H", base, first)[0]
        refused = (("empty", b"", "not a .wem"), ("noise", bytes(rng.getrandbits(8) for _ in range(5000)), "not a .wem"),
                   ("an Ogg file", open(os.path.join(tmp, "q5-48k.ogg"), "rb").read(), "not a .wem"),
                   ("the header alone", base[:94], ""), ("cut in the header", base[:60], "not a Wwise Vorbis file"),
                   ("cut in the setup", base[:first - 50], ""), ("no packets", base[:first], "no audio packets"),
                   ("half a packet", base[:first + size1 - 1], "no audio packets"),
                   ("a PCM wave file", open(wav, "rb").read(100000), "not a Wwise Vorbis file"))
        for what, d, why in refused:
            p = os.path.join(tmp, "bad.wem")
            open(p, "wb").write(d)
            r = run("cut", p, os.path.join(tmp, "no.wem"), -1, 0)
            ok(r.returncode == 3 and why in r.stdout, "%s is refused for its reason: %s%s" % (what, r.stdout, r.stderr[-1500:]))
        # one packet: a file of no frames; a file cut short: what is whole
        for what, d in (("one packet", base[:first + size1]), ("cut short", base[:len(base) * 2 // 3 + 1])):
            p = os.path.join(tmp, "short.wem")
            open(p, "wb").write(d)
            r = run("cut", p, os.path.join(tmp, "short-cut.wem"), -1, 0)
            ok(r.returncode == 0, "%s still cuts: %s%s" % (what, r.stdout, r.stderr[-1500:]))
            hh, _, _, pk, _ = wwv.read_wem(os.path.join(tmp, "short-cut.wem"))
            ok(pk[-1][0] + 2 + len(pk[-1][1]) == hh["data_size"] and (what != "one packet" or (len(pk), hh["frames"]) == (1, 0)), what + ": whole packets")
        r = run("fuzz", fuzz_src, 6000)
        ok(r.returncode == 0, "damaged copies: %s%s" % (r.stdout, r.stderr[-3000:]))
        print("damaged input:", r.stdout.strip())
    print("ok, %d checks" % checks)


if __name__ == "__main__":
    main()
