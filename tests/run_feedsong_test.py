#!/usr/bin/env python3
"""A feedpak as a song archive: tests/run_feedsong_test.py FEEDSONGTEST --tools DIR --pak FILE [--pak ...] [--codebooks FILE]

Each package is made into an archive for both platforms by feedsongtest (which reads it in several ways, from the
package and from the kept sizes, and compares), with and without a cache folder: the same bytes either way. The
archive is then taken apart with rs-rearmed's reader and looked at: the entries a custom song has, manifests that are
JSON and agree with the package's manifest, sound banks that name and start with their audio, textures of the right
size, and audio that decodes (vgmstream-cli) to what the package's Ogg decodes to (sox). Needs the game's codebook
library; without it the test is skipped."""
import argparse, array, json, os, re, shutil, struct, subprocess, sys, tempfile, wave, zipfile

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import run_varc_test as varc  # the archive reader with this repository's keys

checks = 0


def ok(cond, what):
    global checks
    checks += 1
    if not cond:
        sys.exit("FAIL: " + what)


def pcm(path):
    w = wave.open(path)
    return w.getnframes(), array.array("h", w.readframes(w.getnframes()))


def vocals_of(p):
    """A lyrics song file's payload -> [(time, length, text)]; checks that nothing else is in it"""
    ok(p[:16] == bytes(16), "a lyrics file has no beats, phrases or chords")
    n = struct.unpack_from("<I", p, 16)[0]
    out = []
    for k in range(n):
        t, note, ln = struct.unpack_from("<fif", p, 20 + 60 * k)
        out.append((t, ln, p[32 + 60 * k:80 + 60 * k].split(b"\0")[0].decode()))
    return out


def samples_vocals(a, px, tmp):
    """Real lyrics files, written out as a package's lyrics and made again: the same bytes but for the pitch sung"""
    n = 0
    for name in sorted(os.listdir(a.samples)):
        if not name.endswith(".psarc"):
            continue
        plat = "mac" if name.endswith("_m.psarc") else "pc"
        for entry, data, _ in varc.read_all(px, os.path.join(a.samples, name)):
            if not entry.endswith("_vocals.sng"):
                continue
            f = os.path.join(tmp, "s.sng")
            open(f, "wb").write(data)
            subprocess.run([a.vocalstest, "unpack", plat, f, f + ".bin"], check=True, capture_output=True)
            real = bytearray(open(f + ".bin", "rb").read())
            words = [{"t": float(repr(t)) if False else t, "d": ln, "w": w} for t, ln, w in vocals_of(bytes(real))]
            open(os.path.join(tmp, "l.json"), "w").write(json.dumps(words))
            r = subprocess.run([a.vocalstest, "payload", os.path.join(tmp, "l.json"), f + ".made"], capture_output=True, text=True)
            ok(r.returncode == 0, "%s: %s" % (name, r.stdout))
            for k in range(struct.unpack_from("<I", real, 16)[0]):  # the pitch: custom songs' "none"
                struct.pack_into("<i", real, 24 + 60 * k, 254)
            ok(open(f + ".made", "rb").read() == bytes(real), "%s: its lyrics file made again, byte for byte" % name)
            n += 1
    print("%d real lyrics files made again from their own words" % n)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("feedsongtest")
    ap.add_argument("--tools", required=True)
    ap.add_argument("--pak", action="append", default=[])
    ap.add_argument("--vocalstest")
    ap.add_argument("--samples")
    ap.add_argument("--codebooks", default=os.path.expanduser("~/Library/Caches/RS Re-ARMed/stems/codebooks.bin"))
    a = ap.parse_args()
    if not os.path.exists(a.codebooks):
        print("skipped: no codebook library at", a.codebooks)
        return
    if not a.pak:
        print("skipped: no --pak given")
        return
    px = varc.reader(a.tools, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src", "keys.h"))
    with tempfile.TemporaryDirectory() as tmp:
        if a.samples and a.vocalstest:
            samples_vocals(a, px, tmp)
        if a.vocalstest:
            lj = os.path.join(tmp, "odd.json")
            for text_, want in (('[{"t": 2, "d": 1, "w": "b+"}, {"t": "1.5", "w": "a-"}, {"t": -1, "w": "x"}, {"w": "no time"}, 7, {"t": 3, "w": ""}]', "ok 2"),
                                ('{"not": "a list"}', "not a list"), ("[]", "no lyrics"), ("nonsense", "not a list"),
                                ('[{"t": 1, "w": "' + "é" * 60 + '+"}]', "ok 1")):
                open(lj, "w").write(text_)
                r = subprocess.run([a.vocalstest, "payload", lj, lj + ".bin"], capture_output=True, text=True)
                ok(want in r.stdout, "odd lyrics %r: %s" % (text_[:30], r.stdout))
            got = vocals_of(open(lj + ".bin", "rb").read())
            ok(len(got[0][2].encode()) < 48 and got[0][2].endswith("+") and got[0][2].startswith("é"), "a long syllable is cut at a character and keeps its line end")
            open(lj, "w").write(json.dumps([{"t": i * 0.5, "d": 0.2, "w": "la%d" % i} for i in range(300)]))
            r = subprocess.run([a.vocalstest, "fuzz", lj, "3000"], capture_output=True, text=True)
            ok(r.returncode == 0, "damaged lyrics: %s%s" % (r.stdout, r.stderr[-2000:]))
        for pak in a.pak:
            z = zipfile.ZipFile(pak)
            text = z.read("manifest.yaml").decode()
            title = re.search(r"^title:\s*(.+)$", text, re.M).group(1).strip().strip("'\"")
            artist = re.search(r"^artist:\s*(.+)$", text, re.M).group(1).strip().strip("'\"")
            for plat, adir, sdir in (("mac", "audio/mac/", "songs/bin/macos/"), ("pc", "audio/windows/", "songs/bin/generic/")):
                outs = []
                for cache in (os.path.join(tmp, "cache"), "-"):
                    out = os.path.join(tmp, "out-%d.psarc" % len(outs))
                    r = subprocess.run([a.feedsongtest, pak, plat, cache, a.codebooks, out], capture_output=True, text=True)
                    ok(r.returncode == 0 and r.stdout.strip().endswith("ok"), "%s (%s): %s%s" % (pak, plat, r.stdout, r.stderr[-3000:]))
                    outs.append(open(out, "rb").read())
                said = r.stdout
                ok(outs[0] == outs[1], "the same archive with and without kept sizes")
                key = re.search(r"^key (\w+)$", said, re.M).group(1)
                ents = varc.read_all(px, out)
                by = dict((n, d) for n, d, _ in ents)
                ok(len(by) == len(ents), "no name twice")
                ok(by.get("appid.appid") == b"248750", "the free DLC's AppID")
                for n in ("gamexblocks/nsongs/%s.xblock" % key, "%s_aggregategraph.nt" % key, "songs/arr/%s_showlights.xml" % key,
                          "flatmodels/rs/rsenumerable_root.flat", "flatmodels/rs/rsenumerable_song.flat",
                          adir + "song_%s.bnk" % key, adir + "song_%s_preview.bnk" % key):
                    ok(n in by and by[n], "%s is there" % n)
                for px_, size in ((64, 2176), (128, 8320), (256, 32896)):
                    d = by.get("gfxassets/album_art/album_%s_%d.dds" % (key, px_), b"")
                    ok(len(d) == size and d[:4] == b"DDS ", "album art %d" % px_)
                hsan = json.loads(by["manifests/songs_dlc_%s/songs_dlc_%s.hsan" % (key, key)].decode("utf-8-sig"))
                arrs = [e["Attributes"] for e in hsan["Entries"].values()]
                played = [x for x in arrs if "SongName" in x]  # (the vocals' line has no song name)
                ok(played and all(x["SongName"] == title and x["ArtistName"] == artist for x in played), "the song list manifest names the song")
                manifests = [n for n in by if n.startswith("manifests/") and n.endswith(".json")]
                sngs = [n for n in by if n.startswith(sdir) and n.endswith(".sng")]
                ok(len(manifests) == len(arrs) == len(sngs) > 0, "a manifest and a song file for each arrangement listed")
                for n in manifests:
                    m = json.loads(by[n].decode("utf-8-sig"))
                    at = list(m["Entries"].values())[0]["Attributes"]
                    ok(at["PersistentID"] in hsan["Entries"], n + ": listed by its persistent ID")
                    if n.endswith("_vocals.json"):
                        continue
                    ok(at["SongKey"].lower() == key and at["Tones"] and at["Tone_Base"] in [t["Key"] for t in at["Tones"]],
                       "%s: its key and a tone it starts with" % n)
                    ok(adir.split("/")[1] in ("mac", "windows") and len(at["PhraseIterations"]) > 0 and len(at["Sections"]) > 0, n + ": phrases and sections")
                # the package's lyrics are the song's vocals, word for word
                has = "lyrics.json" in z.namelist() and re.search(r"^lyrics:", text, re.M)
                voc = [n for n in sngs if n.endswith("_vocals.sng")]
                ok(bool(voc) == bool(has), "a vocals song file when the package has lyrics, and only then")
                if voc and a.vocalstest:
                    f = os.path.join(tmp, "v.sng")
                    open(f, "wb").write(by[voc[0]])
                    subprocess.run([a.vocalstest, "unpack", plat, f, f + ".bin"], check=True, capture_output=True)
                    got = vocals_of(open(f + ".bin", "rb").read())
                    want = sorted(json.loads(z.read("lyrics.json")), key=lambda w: w["t"])
                    ok(len(got) == len(want), "%d syllables, the package has %d" % (len(got), len(want)))
                    ok(all(abs(g[0] - w["t"]) < 1e-4 and g[2] == w["w"] for g, w in zip(got, want)), "each at its time with its text")
                # the banks name their audio and start with it; the audio is the package's
                wems = sorted(n for n in by if n.startswith(adir) and n.endswith(".wem"))
                ok(len(wems) == 2, "two audio files")
                for bnk, ogg in ((adir + "song_%s.bnk" % key, "stems/full.ogg"), (adir + "song_%s_preview.bnk" % key, "preview.ogg")):
                    d = by[bnk]
                    ok(d[:4] == b"BKHD" and d[36:40] == b"DIDX", bnk + ": a sound bank")
                    fid, off, size = struct.unpack_from("<III", d, 44)
                    wem = by.get(adir + "%d.wem" % fid)
                    ok(wem is not None and d[64:64 + size] == wem[:size], bnk + ": names its audio and starts with it")
                    if ogg in z.namelist() and plat == "mac":
                        open(os.path.join(tmp, "a.wem"), "wb").write(wem)
                        open(os.path.join(tmp, "a.ogg"), "wb").write(z.read(ogg))
                        subprocess.run(["vgmstream-cli", "-o", os.path.join(tmp, "a.wav"), os.path.join(tmp, "a.wem")], check=True, capture_output=True)
                        subprocess.run(["sox", os.path.join(tmp, "a.ogg"), "-b", "16", os.path.join(tmp, "b.wav")], check=True, capture_output=True)
                        (n1, x), (n2, y) = pcm(os.path.join(tmp, "a.wav")), pcm(os.path.join(tmp, "b.wav"))
                        ok(n1 == n2 and max(abs(p - q) for p, q in zip(x, y)) <= 2, "%s plays as the package's %s" % (bnk, ogg))
            print("%s: %s by %s, key %s, %d arrangements; %s" % (os.path.basename(pak), title, artist, key, len(arrs),
                                                                    re.search(r"^note (.*)$", said, re.M).group(1) or "nothing left out"))
    print("ok, %d checks" % checks)


if __name__ == "__main__":
    main()
