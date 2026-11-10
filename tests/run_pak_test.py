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
arrangement lead name Lead type - drums 0 capo 0 cents 0.0 tuning 0 0 0 0 0 0
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
                     "arrangement lead7 name lead7 type guitar drums 0 capo 2 cents -12.5 tuning -2 0 0 0 0 0 0",
                     "arrangement bass name Bass (5) type bass drums 0 capo 0 cents 0.0 tuning 0 0 0 0 0",
                     "arrangement drums1 name Drums type drums drums 1 capo 0 cents 0.0 tuning 0 0 0 0 0 0",
                     "arrangement score name score type - drums 0 capo 0 cents 0.0 tuning 0 0 0 0 0 0",
                     "stem full codec vorbis on 0", "stem guitar codec opus on 1", "stem click codec pcm on 1", "stem odd codec - on 0",
                     "file stem stems/guitar.audio size 70000 crc %08x" % zlib.crc32(FILES["stems/guitar.audio"]),
                     "file stem stems/odd.xyz size 0 crc 00000000", "file cover cover.png size 103",
                     "file chart arrangements/bass.jsonc size 13"):
            ok(line in s, "extended: %r in\n%s" % (line, s))
        ok("not-a-chart" not in s and s.count("file chart") == 2, "a drum part and a notation-only arrangement have no chart")

        bad = [("no-manifest", None, "no manifest.yaml"),
               ("no-title", MINIMAL.replace('title: "Black Hole Sun"\n', ""), "no title"),
               ("no-artist", MINIMAL.replace('artist: "Soundgarden"\n', ""), "no artist"),
               ("no-arrangements", MINIMAL.replace("arrangements:", "arrangements: []\nold:"), "no arrangements"),
               ("no-stems", MINIMAL.split("stems:")[0], "no stems"),
               ("dotdot", MINIMAL.replace("stems/full.ogg", "../outside.ogg"), "not a path inside the package"),
               ("absolute", MINIMAL.replace("arrangements/lead.json", "/etc/passwd"), "not a path inside the package"),
               ("backslash", MINIMAL + "cover: art\\\\cover.png\n", "not a path inside the package"),
               ("drive", MINIMAL + "cover: 'C:cover.png'\n", "not a path inside the package"),
               ("three-strings", MINIMAL.replace("[0, 0, 0, 0, 0, 0]", "[0, 0, 0]"), "tuning of 3 strings"),
               ("tuning-words", MINIMAL.replace("[0, 0, 0, 0, 0, 0]", "[E, A, D, G, B, E]"), "not semitones"),
               ("not-yaml", "title: [unclosed\nartist: x\n", "manifest.yaml:"),
               ("a-list", "- title\n- artist\n", "not a mapping"),
               ("stem-no-file", MINIMAL.replace("    file: stems/full.ogg\n", ""), "no id or file")]
        for tag, manifest, why in bad:
            for f in make(tmp, tag, manifest, ["arrangements/lead.json", "stems/full.ogg"]):
                made, said_ = run(f)
                ok(not made and why in said_, "%s (%s) is refused for its reason: %s" % (tag, os.path.basename(f), said_))
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
