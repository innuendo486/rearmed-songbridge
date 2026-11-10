#!/usr/bin/env python3
"""The feedpak reader's test: tests/run_pak_test.py PAKTEST [--pak FILE ...]

Packages are made here in the three forms a feedpak has (a folder, a zip with stored files, a zip with deflated
ones); paktest must say the same of each, and what the manifest written here means. Packages the reader must refuse
are checked for the reason, and damaged zips must not crash it. With --pak, each real .feedpak is read too and every
file the manifest names compared with Python's zipfile."""
import argparse, os, subprocess, sys, tempfile, zipfile, zlib

checks = 0


def ok(cond, what):
    global checks
    checks += 1
    if not cond:
        sys.exit("FAIL: " + what)


MINIMAL = """feedpak_version: "1.0.0"
title: "Black Hole Sun"
artist: "Soundgarden"
duration: 320.5
arrangements:
  - id: lead
    name: Lead
    file: arrangements/lead.json
    tuning: [0, 0, 0, 0, 0, 0]
    capo: 0
stems:
  - id: full
    file: stems/full.ogg
    default: true
"""

# by hand: comments, quotes, flow and block lists, booleans as words, nulls, keys the reader does not know
EXTENDED = """# a hand-edited manifest
feedpak_version: 1.18.0
title: 'Été — "Live"'
artist: Motörhead
album: ~
album_artist: "Various"
year: 2003
mbid: 0a8a6e7c-4b5e-4d6f-9f1e-2b3c4d5e6f70
duration: 210.596
some_future_key: {nested: [1, 2, 3]}
cover: cover.png
preview: preview.ogg
lyrics: lyrics.json
song_timeline: song_timeline.json
arrangements:
- id: lead7
  file: arrangements/lead7.json
  tuning: [-2, 0, 0, 0, 0, 0, 0]
  capo: 2
  centOffset: -12.5
  type: guitar
- id: bass
  name: "Bass (5)"
  file: arrangements/bass.jsonc
  tuning:
  - 0
  - 0
  - 0
  - 0
  - 0
  type: bass
- {id: drums1, name: Drums, type: drums, drum_tab: drum_tab.json, file: arrangements/not-a-chart.json}
- id: score
  notation: notation_score.json
- {id: vocals, name: Vocals, type: Vocals, file: arrangements/lead7.json}
stems:
- {id: full, file: stems/full.ogg, default: false}
- id: guitar
  file: stems/guitar.audio
  codec: Opus
  default: "on"
- id: click
  file: stems/click.wav
  default: Yes
- id: odd
  file: stems/odd.xyz
"""

FILES = {"arrangements/lead.json": b'{"notes": []}', "stems/full.ogg": b"OggS" + bytes(range(256)) * 300,
         "arrangements/lead7.json": b'{"notes": [{"t": 1, "s": 6, "f": 3}]}', "arrangements/bass.jsonc": b"// by hand\n{}",
         "stems/guitar.audio": b"x" * 70000, "stems/click.wav": b"RIFF", "stems/odd.xyz": b"", "cover.png": b"\x89PNG" + b"\0" * 99,
         "preview.ogg": b"OggS", "lyrics.json": b"[]", "song_timeline.json": b"{}"}

WANT_MINIMAL = """title Black Hole Sun
artist Soundgarden
album -
album_artist -
mbid -
isrc -
year 0
duration 320.500
arrangement lead name Lead type - drums 0 vocals 0 capo 0 cents 0.0 tuning 0 0 0 0 0 0
file chart arrangements/lead.json size 13 crc %08x
stem full codec vorbis on 1
file stem stems/full.ogg size 76804 crc %08x
absent -1 -1
""" % (zlib.crc32(FILES["arrangements/lead.json"]), zlib.crc32(FILES["stems/full.ogg"]))


def make(tmp, tag, manifest, names):
    """The package in its three forms: [path]"""
    d = os.path.join(tmp, tag + ".feedpak")
    files = dict((n, FILES[n]) for n in names)
    if manifest is not None:
        files["manifest.yaml"] = manifest.encode()
    for n, data in files.items():
        p = os.path.join(d, n)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        open(p, "wb").write(data)
    os.makedirs(d, exist_ok=True)
    out = [d]
    for kind, method in (("stored", zipfile.ZIP_STORED), ("deflated", zipfile.ZIP_DEFLATED)):
        z = os.path.join(tmp, "%s-%s.zip" % (tag, kind))
        with zipfile.ZipFile(z, "w", method) as f:
            f.writestr("arrangements/", b"")  # a folder's own entry, as some zip writers add
            for n, data in files.items():
                f.writestr(n, data)
            f.comment = b"a comment after the end record"
        out.append(z)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("paktest")
    ap.add_argument("--pak", action="append", default=[])
    a = ap.parse_args()

    def run(path):
        r = subprocess.run([a.paktest, path], capture_output=True, text=True)
        ok(r.returncode in (0, 3), "%s: paktest: %s%s" % (path, r.stdout, r.stderr[-3000:]))
        return r.returncode == 0, r.stdout

    with tempfile.TemporaryDirectory() as tmp:
        forms = make(tmp, "minimal", MINIMAL, ["arrangements/lead.json", "stems/full.ogg"])
        for f in forms:
            made, said = run(f)
            ok(made and said == WANT_MINIMAL, "%s reads as written:\n%s" % (os.path.basename(f), said))

        forms = make(tmp, "extended", EXTENDED, list(FILES))
        said = [run(f) for f in forms]
        ok(all(m for m, _ in said) and len(set(s for _, s in said)) == 1, "the three forms read the same:\n" + said[0][1])
        s = said[0][1]
        for line in ('title Été — "Live"', "artist Motörhead", "album -", "album_artist Various", "year 2003", "duration 210.596",
                     "mbid 0a8a6e7c-4b5e-4d6f-9f1e-2b3c4d5e6f70",
                     "arrangement lead7 name lead7 type guitar drums 0 vocals 0 capo 2 cents -12.5 tuning -2 0 0 0 0 0 0",
                     "arrangement bass name Bass (5) type bass drums 0 vocals 0 capo 0 cents 0.0 tuning 0 0 0 0 0",
                     "arrangement drums1 name Drums type drums drums 1 vocals 0 capo 0 cents 0.0 tuning 0 0 0 0 0 0",
                     "arrangement score name score type - drums 0 vocals 0 capo 0 cents 0.0 tuning 0 0 0 0 0 0",
                     "arrangement vocals name Vocals type Vocals drums 0 vocals 1 capo 0 cents 0.0 tuning 0 0 0 0 0 0",
                     "stem full codec vorbis on 0", "stem guitar codec opus on 1", "stem click codec pcm on 1", "stem odd codec - on 0",
                     "file stem stems/guitar.audio size 70000 crc %08x" % zlib.crc32(FILES["stems/guitar.audio"]),
                     "file stem stems/odd.xyz size 0 crc 00000000", "file cover cover.png size 103",
                     "file chart arrangements/bass.jsonc size 13"):
            ok(line in s, "extended: %r in\n%s" % (line, s))
        ok("not-a-chart" not in s and s.count("file chart") == 2, "a drum part, a notation-only arrangement and the vocals have no chart")

        # nothing to play: refused, for the reason
        bad = [("no-manifest", None, "no manifest.yaml"),
               ("no-arrangements", MINIMAL.replace("arrangements:", "arrangements: []\nold:"), "no arrangements"),
               ("arrangements-a-word", MINIMAL.split("arrangements:")[0] + "arrangements: none\nstems:" + MINIMAL.split("stems:")[1], "no arrangements"),
               ("no-stems", MINIMAL.split("stems:")[0], "no stems"),
               ("only-stem-outside", MINIMAL.replace("stems/full.ogg", "../outside.ogg"), "no stems"),
               ("only-stem-no-file", MINIMAL.replace("    file: stems/full.ogg\n", ""), "no stems"),
               ("not-yaml", "title: [unclosed\nartist: x\n", "manifest.yaml:"),
               ("a-list", "- title\n- artist\n", "not a mapping")]
        for tag, manifest, why in bad:
            for f in make(tmp, tag, manifest, ["arrangements/lead.json", "stems/full.ogg"]):
                made, said_ = run(f)
                ok(not made and why in said_, "%s (%s) is refused for its reason: %s" % (tag, os.path.basename(f), said_))

        # odd but playable: read, with what was left out or repaired said
        more_stems = "  - id: outside\n    file: ../outside.ogg\n  - file: stems/full.ogg\n  - id: nofile\n  - just a word\n"
        odd = [("no-title", MINIMAL.replace('title: "Black Hole Sun"\n', ""), ["artist Soundgarden", "warning no title: named by its file"], []),
               ("no-artist", MINIMAL.replace('artist: "Soundgarden"\n', ""), ["artist Unknown Artist", "warning no artist"], []),
               ("stems-some-bad", MINIMAL + more_stems,
                ["stem full codec vorbis on 1", "stem full codec vorbis on 0", "warning stem 2: file is not a path inside the package",
                 "warning stem 4 has no file: left out", "warning stem 5 has no file: left out"], ["stem outside", "stem nofile"]),
               ("chart-outside", MINIMAL.replace("arrangements/lead.json", "/etc/passwd"),
                ["arrangement lead name Lead", "warning arrangement lead: file is not a path inside the package: /etc/passwd"], ["file chart"]),
               ("cover-backslash", MINIMAL + "cover: art\\\\cover.png\npreview: 'C:preview.ogg'\n",
                ["warning the manifest: cover is not a path inside the package", "warning the manifest: preview is not a path inside the package"],
                ["file cover", "file preview"]),
               ("three-strings", MINIMAL.replace("[0, 0, 0, 0, 0, 0]", "[0, 0, 0]"), ["tuning 0 0 0 0 0 0\n", "a tuning that is not 4 to 8 semitone steps"], []),
               ("tuning-words", MINIMAL.replace("[0, 0, 0, 0, 0, 0]", "[E, A, D, G, B, E]"), ["tuning 0 0 0 0 0 0\n", "a tuning that is not 4 to 8 semitone steps"], []),
               ("tuning-a-word", MINIMAL.replace("[0, 0, 0, 0, 0, 0]", "standard"), ["tuning 0 0 0 0 0 0\n", "a tuning that is no list"], []),
               ("tuning-floats", MINIMAL.replace("[0, 0, 0, 0, 0, 0]", "[-2.0, 0.0, 0, 0, 0, '0']"), ["tuning -2 0 0 0 0 0\n"], ["warning"]),
               ("arrangement-odd", MINIMAL.replace("  - id: lead\n", "  - 17\n  - name: Nameless\n    file: arrangements/lead.json\n  - id: lead\n"),
                ["warning arrangement 1 is not a mapping: left out", "arrangement arrangement-2 name Nameless", "warning arrangement 2 has no id",
                 "arrangement lead name Lead"], []),
               ("numbers-as-words", MINIMAL.replace("duration: 320.5", "duration: about five minutes\nyear: MCMXCIV").replace("capo: 0", "capo: second fret"),
                ["duration 0.000", "year 0", "capo 0"], [])]
        for tag, manifest, want, unwanted in odd:
            for f in make(tmp, tag, manifest, ["arrangements/lead.json", "stems/full.ogg"]):
                made, said_ = run(f)
                ok(made, "%s (%s) is read: %s" % (tag, os.path.basename(f), said_))
                for w in want:
                    ok(w in said_, "%s (%s): %r in\n%s" % (tag, os.path.basename(f), w, said_))
                for w in unwanted:
                    ok(w not in said_, "%s (%s): no %r in\n%s" % (tag, os.path.basename(f), w, said_))
        made, said_ = run(os.path.join(tmp, "no-title.feedpak"))
        ok("title no-title\n" in said_, "a song with no title is named by its file: " + said_)

        # a zip of the package's folder instead of its files, and names that differ in case from the manifest's
        wrapped = os.path.join(tmp, "wrapped.feedpak")
        with zipfile.ZipFile(wrapped, "w", zipfile.ZIP_DEFLATED) as f:
            f.writestr("__MACOSX/My Song.feedpak/manifest.yaml", b"junk a Mac's zip adds")
            f.writestr("My Song.feedpak/Manifest.YAML", MINIMAL)
            f.writestr("My Song.feedpak/Arrangements/Lead.JSON", FILES["arrangements/lead.json"])
            f.writestr("My Song.feedpak/stems/FULL.ogg", FILES["stems/full.ogg"])
        made, said_ = run(wrapped)
        ok(made and "warning the package is in the zip's folder My Song.feedpak/" in said_, "a zip of the folder is read: " + said_)
        ok(said_.replace("warning the package is in the zip's folder My Song.feedpak/\n", "") == WANT_MINIMAL, "and reads as the plain one:\n" + said_)
        two = os.path.join(tmp, "two.feedpak")
        with zipfile.ZipFile(two, "w") as f:
            f.writestr("a/manifest.yaml", MINIMAL)
            f.writestr("b/manifest.yaml", MINIMAL)
        made, said_ = run(two)
        ok(not made and "no manifest.yaml" in said_, "a zip of two packages is none: " + said_)

        junk = os.path.join(tmp, "junk.feedpak")
        open(junk, "wb").write(os.urandom(4000))
        made, said_ = run(junk)
        ok(not made and "not a zip file" in said_, "noise is no zip: " + said_)
        open(junk, "wb").close()
        ok(not run(junk)[0], "an empty file is no package")
        ok(not run(os.path.join(tmp, "nowhere.feedpak"))[0], "a path that is not there")

        r = subprocess.run([a.paktest, os.path.join(tmp, "extended-deflated.zip"), "--fuzz", "1500"], capture_output=True, text=True)
        ok(r.returncode == 0, "damaged copies: %s%s" % (r.stdout, r.stderr[-3000:]))
        print("damaged copies:", r.stdout.strip())

        for pak in a.pak:
            made, said_ = run(pak)
            ok(made, "%s: %s" % (pak, said_))
            z = zipfile.ZipFile(pak)
            n = 0
            for line in said_.splitlines():
                if line.startswith("file "):
                    _, _, rel, _, size, _, crc = line.split(" ")
                    data = z.read(rel)
                    ok(len(data) == int(size) and "%08x" % zlib.crc32(data) == crc, "%s: %s as zipfile reads it" % (pak, rel))
                    n += 1
            print("%s: %s by %s, %d files as zipfile reads them" % (os.path.basename(pak), said_.splitlines()[0][6:], said_.splitlines()[1][7:], n))
    print("ok, %d checks" % checks)


if __name__ == "__main__":
    main()
