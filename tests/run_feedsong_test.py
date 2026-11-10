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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("feedsongtest")
    ap.add_argument("--tools", required=True)
    ap.add_argument("--pak", action="append", default=[])
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
                ok(arrs and all(x["SongName"] == title and x["ArtistName"] == artist for x in arrs), "the song list manifest names the song")
                manifests = [n for n in by if n.startswith("manifests/") and n.endswith(".json")]
                sngs = [n for n in by if n.startswith(sdir) and n.endswith(".sng")]
                ok(len(manifests) == len(arrs) == len(sngs) > 0, "a manifest and a song file for each arrangement listed")
                for n in manifests:
                    m = json.loads(by[n].decode("utf-8-sig"))
                    at = list(m["Entries"].values())[0]["Attributes"]
                    ok(at["SongKey"].lower() == key and at["Tones"] and at["Tone_Base"] in [t["Key"] for t in at["Tones"]],
                       "%s: its key and a tone it starts with" % n)
                    ok(at["PersistentID"] in hsan["Entries"], n + ": listed by its persistent ID")
                    ok(adir.split("/")[1] in ("mac", "windows") and len(at["PhraseIterations"]) > 0 and len(at["Sections"]) > 0, n + ": phrases and sections")
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
