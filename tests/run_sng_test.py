#!/usr/bin/env python3
"""The chart reader's and song file writer's test:
  tests/run_sng_test.py SNGTEST [--tools DIR --samples DIR] [--feedpak FILE]... [--fuzz N] [--verbose]

SNGTEST is tests/sngtest.c built with the sanitizers (tests/build_sng_test.sh does both). This file has its own
reader and writer of the song file's plain payload, so that the C writer is checked by something that is not it.

With --samples (a folder of real archives; --tools is rs-rearmed's tools folder, whose archive reader is used the way
tests/run_varc_test.py uses it), for every arrangement song file in them:
  1. layout: the payload read and written again by this file's reader is the same bytes; packed and unpacked by
     sngtest it is too.
  2. round trip: the song file is turned into an arrangement JSON the way the format's reference converter maps the
     game's chart to it (the field mapping of slopsmith's lib/song.py, and its newer fields for what that one lost),
     sngtest builds a song file from the JSON, and the two are compared field by field. Fields that the format
     carries must match exactly; the derived ones are reported with how many match (see the head of src/sng.c).
With --feedpak (a real feedpak, a zip), every arrangement in it is built, read back and counted against the manifest.
Always: made-up charts for every repair the reader makes, bad input that must be refused, and a fuzz run."""
import argparse, collections, json, os, re, struct, subprocess, sys, tempfile, zipfile

sys.dont_write_bytecode = True  # importing run_varc_test leaves nothing in the repository
checks = 0
verbose = False


def ok(cond, what):
    global checks
    checks += 1
    if not cond:
        sys.exit("FAIL: " + what)


# --- the song file's payload ---------------------------------------------------------------------------------------
# a record is a list of (name, format): a struct code, ("arr", record) for a counted list, ("fix", n, record)
BEND = [("time", "f"), ("step", "f"), ("pad", "3s"), ("unk", "b")]
BEAT = [("time", "f"), ("measure", "H"), ("beat", "H"), ("pi", "I"), ("mask", "I")]
PHRASE = [("solo", "b"), ("disparity", "b"), ("ignore", "b"), ("pad", "1s"), ("maxDifficulty", "I"), ("links", "I"), ("name", "32s")]
TEMPLATE = [("mask", "I"), ("frets", "6b"), ("fingers", "6b"), ("notes", "6i"), ("name", "32s")]
BEND32 = [("bends", ("fix", 32, BEND)), ("count", "I")]
CHORDNOTE = [("mask", "6I"), ("bends", ("fix", 6, BEND32)), ("slideTo", "6b"), ("slideUnpitchTo", "6b"), ("vibrato", "6h")]
VOCAL = [("time", "f"), ("note", "i"), ("length", "f"), ("lyric", "48s")]
PI = [("phraseId", "I"), ("time", "f"), ("endTime", "f"), ("difficulty", "3I")]
PEI = [("phraseId", "I"), ("difficulty", "I"), ("empty", "I"), ("levelJump", "b"), ("redundant", "h"), ("pad", "1s")]
NLD = [("levelBreak", "i"), ("phrases", ("arr", [("v", "I")]))]
ACTION = [("time", "f"), ("name", "256s")]
TONE = [("time", "f"), ("id", "I")]
SECTION = [("name", "32s"), ("number", "I"), ("startTime", "f"), ("endTime", "f"), ("startPi", "I"), ("endPi", "I"), ("stringMask", "36s")]
ANCHOR = [("time", "f"), ("endTime", "f"), ("firstNoteTime", "f"), ("lastNoteTime", "f"), ("fret", "b"), ("pad", "3s"), ("width", "i"), ("pi", "I")]
ANCHOREXT = [("time", "f"), ("fret", "b"), ("pad", "7s")]
FP = [("chordId", "I"), ("startTime", "f"), ("endTime", "f"), ("firstNoteTime", "f"), ("lastNoteTime", "f")]
NOTE = [("mask", "I"), ("flags", "I"), ("hash", "I"), ("time", "f"), ("string", "b"), ("fret", "b"), ("anchorFret", "b"), ("anchorWidth", "b"),
        ("chordId", "i"), ("chordNoteId", "i"), ("phraseId", "i"), ("pi", "i"), ("fp", "2h"), ("next", "h"), ("prev", "h"), ("parent", "h"),
        ("slideTo", "b"), ("slideUnpitchTo", "b"), ("leftHand", "b"), ("tap", "b"), ("pick", "b"), ("slap", "b"), ("pluck", "b"),
        ("vibrato", "h"), ("sustain", "f"), ("maxBend", "f"), ("bends", ("arr", BEND))]
LEVEL = [("difficulty", "I"), ("anchors", ("arr", ANCHOR)), ("anchorExts", ("arr", ANCHOREXT)), ("handshapes", ("arr", FP)),
         ("arpeggios", ("arr", FP)), ("notes", ("arr", NOTE)), ("avgNotes", ("arr", [("v", "f")])),
         ("notesNoIgnored", ("arr", [("v", "I")])), ("notesInIter", ("arr", [("v", "I")]))]
META = [("maxScore", "d"), ("maxNotes", "d"), ("maxNotesNoIgnored", "d"), ("pointsPerNote", "d"), ("firstBeatLength", "f"), ("startTime", "f"),
        ("capo", "b"), ("date", "32s"), ("part", "h"), ("songLength", "f"), ("tuning", ("arr", [("v", "h")])), ("firstNoteTime", "f"),
        ("firstNoteTime2", "f"), ("maxDifficulty", "i")]
SONG = [("beats", ("arr", BEAT)), ("phrases", ("arr", PHRASE)), ("templates", ("arr", TEMPLATE)), ("chordNotes", ("arr", CHORDNOTE)),
        ("vocals", ("arr", VOCAL)), ("pis", ("arr", PI)), ("peis", ("arr", PEI)), ("nlds", ("arr", NLD)), ("actions", ("arr", ACTION)),
        ("events", ("arr", ACTION)), ("tones", ("arr", TONE)), ("dnas", ("arr", TONE)), ("sections", ("arr", SECTION)),
        ("levels", ("arr", LEVEL)), ("meta", ("one", META))]

M_CHORD, M_OPEN, M_FRETMUTE, M_TREMOLO, M_HARMONIC, M_PALMMUTE, M_SLAP, M_PLUCK = 0x2, 0x4, 0x8, 0x10, 0x20, 0x40, 0x80, 0x100
M_HAMMER, M_PULL, M_SLIDE, M_BEND, M_SUSTAIN, M_TAP, M_PINCH, M_VIBRATO = 0x200, 0x400, 0x800, 0x1000, 0x2000, 0x4000, 0x8000, 0x10000
M_MUTE, M_IGNORE, M_LEFTHAND, M_RIGHTHAND, M_HIGHDENSITY, M_UNPITCHED, M_SINGLE = 0x20000, 0x40000, 0x80000, 0x100000, 0x200000, 0x400000, 0x800000
M_CHORDNOTES, M_DOUBLESTOP, M_ACCENT, M_PARENT, M_CHILD, M_ARPEGGIO, M_PANEL = 0x1000000, 0x2000000, 0x4000000, 0x8000000, 0x10000000, 0x20000000, 0x80000000
WIRE_FLAGS = [("ho", M_HAMMER), ("po", M_PULL), ("hm", M_HARMONIC), ("hp", M_PINCH), ("pm", M_PALMMUTE), ("mt", M_MUTE), ("vb", M_VIBRATO),
              ("tr", M_TREMOLO), ("ac", M_ACCENT), ("tp", M_TAP), ("ln", M_PARENT), ("fhm", M_FRETMUTE), ("plk", M_PLUCK), ("slp", M_SLAP),
              ("ig", M_IGNORE)]


def rd(schema, d, o):
    out = {}
    for name, f in schema:
        if isinstance(f, str):
            f = "<" + f
            v = struct.unpack_from(f, d, o)
            o += struct.calcsize(f)
            out[name] = v[0] if len(v) == 1 else list(v)
        elif f[0] == "arr":
            n, = struct.unpack_from("<I", d, o)
            o += 4
            if n > len(d):
                raise ValueError("a count past the payload")
            a = []
            for _ in range(n):
                v, o = rd(f[1], d, o)
                a.append(v)
            out[name] = a
        elif f[0] == "fix":
            a = []
            for _ in range(f[1]):
                v, o = rd(f[2], d, o)
                a.append(v)
            out[name] = a
        else:
            out[name], o = rd(f[1], d, o)
    return out, o


def wr(schema, v, out):
    for name, f in schema:
        x = v[name]
        if isinstance(f, str):
            out.append(struct.pack("<" + f, *x) if isinstance(x, list) else struct.pack("<" + f, x))
        elif f[0] == "arr":
            out.append(struct.pack("<I", len(x)))
            for e in x:
                wr(f[1], e, out)
        elif f[0] == "fix":
            for e in x:
                wr(f[2], e, out)
        else:
            wr(f[1], x, out)


def parse(d):
    """The payload as a dict; "tail" is what follows the metadata (a 7-string mod's RS7X block), kept as it is"""
    s, o = rd(SONG, d, 0)
    if o != len(d) and d[o:o + 4] != b"RS7X":
        raise ValueError("%d bytes after the metadata" % (len(d) - o))
    s["tail"] = d[o:]
    return s


def emit(s):
    out = []
    wr(SONG, s, out)
    return b"".join(out) + s["tail"]


def cs(b):
    return b.split(b"\0")[0].decode("utf8", "replace")


def f32(x):
    return struct.unpack("<f", struct.pack("<f", x))[0]


def num(x):
    """A 32-bit float as the shortest decimal that reads back as it"""
    for digits in range(1, 10):
        v = float("%.*g" % (digits, x))
        if f32(v) == x:
            return v
    return x


# --- a song file as an arrangement JSON -----------------------------------------------------------------------------
def wire_note(n, t0=None):
    d = {"s": n["string"], "f": n["fret"]}
    if t0 is None:
        d["t"] = num(n["time"])
    if n["sustain"]:
        d["sus"] = num(n["sustain"])
    if n["slideTo"] != -1:
        d["sl"] = n["slideTo"]
    if n["slideUnpitchTo"] != -1:
        d["slu"] = n["slideUnpitchTo"]
    if n["bends"]:
        d["bn"] = num(n["maxBend"])
        d["bnv"] = [{"t": b["time"] - n["time"], "v": num(b["step"])} for b in n["bends"]]
    for key, bit in WIRE_FLAGS:
        if n["mask"] & bit:
            d[key] = True
    if n["leftHand"] != -1:
        d["fg"] = n["leftHand"]
    if n["pick"] == 1:
        d["pkd"] = 1
    return d


def wire_chord(s, n):
    tp = s["templates"][n["chordId"]]
    cn = s["chordNotes"][n["chordNoteId"]] if n["chordNoteId"] != -1 else None
    notes = []
    sustained = cn is not None and any(m & M_SUSTAIN for m in cn["mask"])
    for st in range(6):
        if tp["frets"][st] < 0:
            continue
        d = {"s": st, "f": tp["frets"][st]}
        m = cn["mask"][st] if cn else 0
        if n["sustain"] and (m & M_SUSTAIN or not sustained):
            d["sus"] = num(n["sustain"])
        if cn:
            if cn["slideTo"][st] != -1:
                d["sl"] = cn["slideTo"][st]
            if cn["slideUnpitchTo"][st] != -1:
                d["slu"] = cn["slideUnpitchTo"][st]
            b = cn["bends"][st]
            if b["count"]:
                pts = b["bends"][:b["count"]]
                d["bn"] = num(max(p["step"] for p in pts))
                d["bnv"] = [{"t": p["time"] - n["time"], "v": num(p["step"])} for p in pts]
            if cn["vibrato"][st]:
                d["vb"] = True
        for key, bit in WIRE_FLAGS:
            if m & bit:
                d[key] = True
        notes.append(d)
    # what the chord itself says goes to its notes, where the format has it
    for key, bit, every in (("fhm", M_FRETMUTE, True), ("pm", M_PALMMUTE, True), ("ig", M_IGNORE, True), ("ac", M_ACCENT, False),
                            ("ln", M_PARENT, False)):
        if not n["mask"] & bit:
            continue
        if key == "fhm" and all(x.get("mt") for x in notes):
            continue
        if every or not any(x.get(key) for x in notes):
            for x in notes:
                x[key] = True
    d = {"t": num(n["time"]), "id": n["chordId"], "notes": notes}
    if n["mask"] & M_HIGHDENSITY:
        d["hd"] = True
    return d


def wire_part(s, lv, t0, t1):
    inside = lambda t: t0 <= t < t1
    p = {"notes": [], "chords": [], "anchors": [], "handshapes": []}
    for n in lv["notes"]:
        if inside(n["time"]):
            if n["chordId"] == -1:
                p["notes"].append(wire_note(n))
            else:
                p["chords"].append(wire_chord(s, n))
    p["anchors"] = [{"time": num(a["time"]), "fret": a["fret"], "width": a["width"]} for a in lv["anchors"] if inside(a["time"])]
    for key, arp in (("handshapes", False), ("arpeggios", True)):
        for h in lv[key]:
            if inside(h["startTime"]):
                p["handshapes"].append({"chord_id": h["chordId"], "start_time": num(h["startTime"]), "end_time": num(h["endTime"]), "arp": arp})
    p["handshapes"].sort(key=lambda h: h["start_time"])
    return p


def to_wire(s, name):
    """(arrangement JSON, song timeline JSON) of a parsed song file, with every difficulty as "phrases\""""
    levels = {lv["difficulty"]: lv for lv in s["levels"]}
    flat = {"notes": [], "chords": [], "anchors": [], "handshapes": []}
    phrases = []
    for i, it in enumerate(s["pis"]):
        top = s["phrases"][it["phraseId"]]["maxDifficulty"]
        t1 = it["endTime"] if i + 1 < len(s["pis"]) else float("inf")
        lvls = []
        for d in sorted(levels):
            if d <= top:
                part = wire_part(s, levels[d], it["time"], t1)
                part["difficulty"] = d
                lvls.append(part)
        phrases.append({"start_time": num(it["time"]), "end_time": num(it["endTime"]), "max_difficulty": top, "levels": lvls})
        for k in flat:
            flat[k] += lvls[-1][k]
    tones = {"base": "tone0", "changes": [{"t": num(t["time"]), "name": "tone%d" % t["id"]} for t in s["tones"]]}
    arr = {"name": name, "tuning": [t["v"] for t in s["meta"]["tuning"]], "capo": max(s["meta"]["capo"], 0), **flat,
           "templates": [{"name": cs(t["name"]), "displayName": cs(t["name"]), "arp": bool(t["mask"] & 1), "fingers": t["fingers"],
                          "frets": t["frets"]} for t in s["templates"]],
           "phrases": phrases, "tones": tones, "stats": {"not": "a key of the format"}}
    timeline = {"version": 1,
                "beats": [{"time": num(b["time"]), "measure": b["measure"] if b["beat"] == 0 else -1} for b in s["beats"]],
                "sections": [{"name": cs(x["name"]), "number": x["number"], "time": num(x["startTime"])} for x in s["sections"]]}
    return arr, timeline


# --- sngtest ---------------------------------------------------------------------------------------------------------
class Tool:
    def __init__(self, exe, tmp):
        self.exe, self.tmp, self.n = exe, tmp, 0

    def path(self, tag):
        self.n += 1
        return os.path.join(self.tmp, "%d-%s" % (self.n, tag))

    def run(self, *args, allow=(0,)):
        r = subprocess.run([self.exe] + list(args), capture_output=True, text=True)
        if r.returncode not in allow:
            sys.exit("FAIL: sngtest %s: exit %d\n%s%s" % (" ".join(args)[:300], r.returncode, r.stdout[-2000:], r.stderr[-4000:]))
        return r

    def unpack(self, plat, data):
        src, dst = self.path("in.sng"), self.path("out.bin")
        open(src, "wb").write(data)
        r = self.run("unpack", plat, src, dst, allow=(0, 3))
        return open(dst, "rb").read() if r.returncode == 0 else None

    def pack(self, plat, payload):
        src, dst = self.path("in.bin"), self.path("out.sng")
        open(src, "wb").write(payload)
        self.run("pack", plat, src, dst)
        return open(dst, "rb").read()

    def build(self, plat, arr, timeline=None, length=0, bass=-1):
        """(payload, facts lines) or (None, reason). arr and timeline: dicts, or bytes as they are"""
        a, t, out = self.path("arr.json"), self.path("timeline.json"), self.path("out.sng")
        open(a, "wb").write(arr if isinstance(arr, bytes) else json.dumps(arr).encode())
        if timeline is not None:
            open(t, "wb").write(timeline if isinstance(timeline, bytes) else json.dumps(timeline).encode())
        r = self.run("build", plat, a, t if timeline is not None else "-", out, repr(float(length)), str(bass), allow=(0, 3))
        if r.returncode == 3:
            ok(r.stdout.startswith("none: ") and len(r.stdout) > 8, "a refusal says why")
            return None, r.stdout[6:].strip()
        sng = open(out, "rb").read()
        ok(struct.unpack("<II", sng[:8]) == (0x4a, 3) and sng[-56:] == bytes(56), "header: magic, flags 3; a zero signature")
        last = len(sng) % 0x10000
        tail = sng[len(sng) - last:] if last and len(sng) > 0x10000 else b"xx"
        ok(not (tail[0] & 15 == 8 and tail[0] >> 4 <= 7 and (tail[0] << 8 | tail[1]) % 31 == 0), "the last block does not start as a zlib stream")
        ok(self.unpack("mac" if plat == "pc" else "pc", sng) is None, "the other platform's key does not open it")
        return self.unpack(plat, sng), r.stdout.split("\n")


def facts(lines):
    f = collections.defaultdict(list)
    for l in lines:
        if l:
            k, _, v = l.partition(" ")
            f[k].append(v)
    return f


# --- comparing two song files ---------------------------------------------------------------------------------------
class Tally:
    def __init__(self):
        self.t = collections.OrderedDict()

    def add(self, field, same, n=1):
        a = self.t.setdefault(field, [0, 0])
        a[0] += n if same else 0
        a[1] += n

    def merge(self, other):
        for k, (a, b) in other.t.items():
            x = self.t.setdefault(k, [0, 0])
            x[0] += a
            x[1] += b


# these may differ: derived by rules that the game's own files and the tools' do not share, or not in the format
APPROX = {"note.flags", "note.mask.panel", "anchor.lastNoteTime", "handshape.lastNoteTime", "handshape.firstNoteTime", "pi.easy",
          "pi.medium", "pi.hard", "section.stringMask", "note.mask.slide-with-unpitched", "note.pick", "note.tap", "note.mask.accent-chord",
          "note.mask.chordnotes", "note.chordNotes", "note.mask.doublestop", "note.mask.other", "bend.unk", "meta.startTime",
          "note.parent", "note.mask.child", "note.order", "meta.pointsPerNote", "anchor.firstNoteTime", "beat.measure", "beat.mask", "meta.firstBeatLength"}


def cn_plain(rec):
    return rec is None or (not any(m & ~M_OPEN for m in rec["mask"]) and not any(b["count"] for b in rec["bends"]) and all(x == -1 for x in rec["slideTo"])
                           and all(x == -1 for x in rec["slideUnpitchTo"]) and not any(rec["vibrato"]))


def cn_same(a, b):
    if cn_plain(a) or cn_plain(b):
        return cn_plain(a) and cn_plain(b)
    for st in range(6):
        ma, mb = a["mask"][st] & ~M_FRETMUTE, b["mask"][st] & ~M_FRETMUTE
        if ma != mb or a["slideTo"][st] != b["slideTo"][st] or a["slideUnpitchTo"][st] != b["slideUnpitchTo"][st]:
            return False
        if bool(a["vibrato"][st]) != bool(b["vibrato"][st]) or a["bends"][st]["count"] != b["bends"][st]["count"]:
            return False
        for p, q in zip(a["bends"][st]["bends"][:a["bends"][st]["count"]], b["bends"][st]["bends"]):
            if abs(p["time"] - q["time"]) > 1e-4 or p["step"] != q["step"]:
                return False
    return True


def compare(a, b):
    """a: the real song file, b: the one built from its JSON. A Tally of field -> same, compared."""
    t = Tally()
    t.add("beats.count", len(a["beats"]) == len(b["beats"]))
    for x, y in zip(a["beats"], b["beats"]):
        for k in ("time", "beat", "pi"):
            t.add("beat." + k, x[k] == y[k])
        for k in ("measure", "mask"):
            t.add("beat." + k, x[k] == y[k])
    t.add("templates.count", len(a["templates"]) == len(b["templates"]))
    for x, y in zip(a["templates"], b["templates"]):
        for k in ("mask", "frets", "fingers", "notes", "name"):
            t.add("template." + k, x[k] == y[k])
    t.add("pis.count", len(a["pis"]) == len(b["pis"]))
    for x, y in zip(a["pis"], b["pis"]):
        t.add("pi.time", x["time"] == y["time"])
        t.add("pi.endTime", x["endTime"] == y["endTime"])
        t.add("pi.maxDifficulty", a["phrases"][x["phraseId"]]["maxDifficulty"] == b["phrases"][y["phraseId"]]["maxDifficulty"])
        for i, k in enumerate(("easy", "medium", "hard")):
            t.add("pi." + k, x["difficulty"][i] == y["difficulty"][i])
    t.add("sections.count", len(a["sections"]) == len(b["sections"]))
    for x, y in zip(a["sections"], b["sections"]):
        for k in ("name", "number", "startTime", "endTime", "startPi", "endPi", "stringMask"):
            t.add("section." + k, x[k] == y[k])
    t.add("tones", a["tones"] == b["tones"])
    t.add("levels.count", len(a["levels"]) == len(b["levels"]))
    for la, lb in zip(a["levels"], b["levels"]):
        t.add("level.difficulty", la["difficulty"] == lb["difficulty"])
        t.add("anchors.count", len(la["anchors"]) == len(lb["anchors"]))
        for x, y in zip(la["anchors"], lb["anchors"]):
            for k in ("time", "endTime", "firstNoteTime", "lastNoteTime", "fret", "width", "pi"):
                t.add("anchor." + k, x[k] == y[k])
        t.add("anchorExts.count", len(la["anchorExts"]) == len(lb["anchorExts"]))
        for x, y in zip(la["anchorExts"], lb["anchorExts"]):
            t.add("anchorExt.time", abs(x["time"] - y["time"]) < 1e-4)
            t.add("anchorExt.fret", x["fret"] == y["fret"])
        for key in ("handshapes", "arpeggios"):
            t.add(key + ".count", len(la[key]) == len(lb[key]))
            for x, y in zip(la[key], lb[key]):
                for k in ("chordId", "startTime", "endTime", "firstNoteTime", "lastNoteTime"):
                    t.add("handshape." + k, x[k] == y[k])
        t.add("notes.count", len(la["notes"]) == len(lb["notes"]))
        if len(la["notes"]) != len(lb["notes"]):
            continue
        # notes at one time are in an order of the writer's choosing: by string here
        key = lambda n: (n["time"], n["chordId"] == -1, n["string"])
        na, nb = sorted(la["notes"], key=key), sorted(lb["notes"], key=key)
        t.add("note.order", [key(n) for n in la["notes"]] == [key(n) for n in lb["notes"]])
        ia = {id(n): i for i, n in enumerate(la["notes"])}
        ib = {id(n): i for i, n in enumerate(lb["notes"])}
        ref = lambda notes, i: None if i == -1 else key(notes[i])
        for x, y in zip(na, nb):
            for k in ("time", "string", "fret", "anchorFret", "anchorWidth", "chordId", "pi", "fp", "slideTo", "slideUnpitchTo", "leftHand",
                      "sustain", "maxBend", "vibrato", "slap", "pluck", "flags", "pick", "tap"):
                t.add("note." + k, x[k] == y[k])
            for k in ("next", "prev", "parent"):
                t.add("note." + k, ref(la["notes"], x[k]) == ref(lb["notes"], y[k]))
            t.add("note.bends", len(x["bends"]) == len(y["bends"]) and
                  all(abs(p["time"] - q["time"]) < 1e-4 and p["step"] == q["step"] for p, q in zip(x["bends"], y["bends"])))
            ca = a["chordNotes"][x["chordNoteId"]] if x["chordNoteId"] != -1 else None
            cb = b["chordNotes"][y["chordNoteId"]] if y["chordNoteId"] != -1 else None
            if x["chordId"] != -1:
                t.add("note.chordNotes", cn_same(ca, cb))
                if verbose and not cn_same(ca, cb):
                    print("      chord notes at %.3f level %d: %s vs %s" % (x["time"], la["difficulty"], ca and [hex(m) for m in ca["mask"]],
                                                                         cb and [hex(m) for m in cb["mask"]]))
            d = x["mask"] ^ y["mask"]
            named = (("panel", M_PANEL), ("chordnotes", M_CHORDNOTES), ("doublestop", M_DOUBLESTOP), ("child", M_CHILD))
            for nm, bit in named:
                t.add("note.mask." + nm, not d & bit)
                d &= ~bit
            if x["mask"] & M_UNPITCHED and d == M_SLIDE:
                t.add("note.mask.slide-with-unpitched", False)
                d = 0
            if x["chordId"] != -1 and d == M_ACCENT:
                t.add("note.mask.accent-chord", False)
                d = 0
            t.add("note.mask.other", d == 0)
            if d and verbose:
                print("      mask %08x vs %08x at %.3f level %d chord %d" % (x["mask"], y["mask"], x["time"], la["difficulty"], x["chordId"]))
        for k in ("notesNoIgnored", "notesInIter"):
            t.add("level." + k, la[k] == lb[k])
    ma, mb = a["meta"], b["meta"]
    for k in ("maxScore", "maxNotes", "maxNotesNoIgnored", "firstBeatLength", "capo", "part", "songLength", "tuning", "firstNoteTime",
              "firstNoteTime2", "maxDifficulty", "startTime"):
        t.add("meta." + k, ma[k] == mb[k])
    t.add("meta.pointsPerNote", abs(ma["pointsPerNote"] - mb["pointsPerNote"]) < 1e-3)
    return t


def show(t, indent="  "):
    exact_bad = []
    for k, (same, n) in t.t.items():
        if same != n:
            if k not in APPROX:
                exact_bad.append("%s %d/%d" % (k, same, n))
            print("%s%-34s %7d of %7d match%s" % (indent, k, same, n, "" if k in APPROX else "   <-- must match"))
    return exact_bad


# --- the tests -------------------------------------------------------------------------------------------------------
def samples_test(tool, tools_dir, samples):
    here = os.path.dirname(os.path.abspath(__file__))
    sys.path.insert(0, here)
    import run_varc_test
    px = run_varc_test.reader(tools_dir, os.path.join(here, "..", "src", "keys.h"))
    total, files, refused = Tally(), 0, 0
    for name in sorted(os.listdir(samples)):
        if not name.endswith(".psarc"):
            continue
        plat = "mac" if name.endswith("_m.psarc") else "pc"
        for entry, data, _ in run_varc_test.read_all(px, os.path.join(samples, name)):
            if not entry.endswith(".sng") or entry.endswith("_vocals.sng"):
                continue
            payload = tool.unpack(plat, data)
            ok(payload is not None, "%s: %s opens with the %s key" % (name, entry, plat))
            s = parse(payload)
            ok(emit(s) == payload, "%s: %s: read and written again, the same bytes" % (name, entry))
            ok(tool.unpack(plat, tool.pack(plat, payload)) == payload, "%s: %s: packed and unpacked, the same bytes" % (name, entry))
            files += 1
            arr, timeline = to_wire(s, entry)
            bass = int("bass" in entry)
            built, info = tool.build(plat, arr, timeline, s["meta"]["songLength"], bass)
            if len(arr["tuning"]) > 6:
                ok(built is None and "strings" in info, "%s: %s: seven strings are refused: %s" % (name, entry, info))
                print("%s %s: refused as it must be (%s)" % (name, os.path.basename(entry), info))
                refused += 1
                continue
            ok(built is not None, "%s: %s: built from its JSON: %s" % (name, entry, info))
            b = parse(built)
            ok(emit(b) == built and not b["tail"], "%s: %s: what was built reads back whole" % (name, entry))
            f = facts(info)
            ok(f["repairs"][0].startswith("0"), "%s: %s: nothing to repair in a converted chart: %s" % (name, entry, f["repairs"]))
            ok(f["made"] == [""], "%s: %s: and nothing to make up: %s" % (name, entry, f["made"]))
            t = compare(s, b)
            print("%s %s: %d levels, %d iterations, %d notes at the top" % (name, os.path.basename(entry), len(s["levels"]), len(s["pis"]),
                                                                           int(s["meta"]["maxNotes"])))
            bad = show(t, "    ") if verbose else [k for k, (x, n) in t.t.items() if x != n and k not in APPROX]
            ok(not bad, "%s: %s: fields that must match differ: %s" % (name, entry, bad))
            # the manifest's facts against the song file itself
            notes = [n for lv in s["levels"] for n in lv["notes"]]
            ok(int(f["max_difficulty"][0].split()[0]) == len(s["levels"]) - 1, "facts: max difficulty")
            ok(len(f["iter"]) == len(s["pis"]) and len(f["section"]) == len(s["sections"]), "facts: iterations and sections")
            ok(len(f["template"]) == len(s["templates"]), "facts: templates")
            m = re.match(r"(\d+) chords (\d+) ignored (\d+) easy (\d+) medium (\d+) hard (\d+)", f["notes"][0])
            ok(int(m.group(1)) + int(m.group(2)) == int(s["meta"]["maxNotes"]) == int(m.group(6)), "facts: notes at the top are the metadata's")
            ok(int(m.group(3)) == int(s["meta"]["maxNotes"] - s["meta"]["maxNotesNoIgnored"]), "facts: ignored notes")
            tech = int(f["notes"][0].split()[-1], 16)
            ok(bool(tech & 1) == any(n["bends"] for n in notes), "facts: bends")
            ok(bool(tech & 2) == any(n["slideTo"] != -1 for n in notes), "facts: slides")
            total.merge(t)
    print("%d arrangement song files: layout exact; %d round trips compared, %d refused (seven strings)" % (files, files - refused, refused))
    print("round trip, all files together (fields not listed match everywhere):")
    show(total)
    return files


def feedpak_test(tool, path):
    import zlib
    z = zipfile.ZipFile(path)
    manifest = z.read("manifest.yaml").decode()
    # the manifest's arrangements, as much YAML as this needs
    arrs, cur, top = [], None, ""
    for line in manifest.split("\n"):
        if line and line[0] not in " -":
            top, cur = line.split(":")[0], None
        elif top == "arrangements" and line.startswith("- "):
            cur = {}
            arrs.append(cur)
            line = "  " + line[2:]
        if cur is not None and re.match(r"  (id|file|type|note_count|event_count): ", line):
            k, v = line.strip().split(": ", 1)
            cur[k] = v.strip()
    arrs = [a for a in arrs if "file" in a]
    ok(arrs, "%s: the manifest names arrangements" % path)
    dur = float(re.search(r"^duration: ([\d.]+)", manifest, re.M).group(1))
    tl = re.search(r"^song_timeline: (\S+)", manifest, re.M)
    timeline = z.read(tl.group(1)) if tl else None
    print("feedpak %s (%.1f s):" % (os.path.basename(path), dur))
    for a in arrs:
        raw = z.read(a["file"])
        j = json.loads(raw)
        for plat in ("mac", "pc"):
            built, info = tool.build(plat, raw, timeline, dur, int(a.get("type") == "bass"))
            if not j.get("notes") and not j.get("chords") and not j.get("phrases"):
                ok(built is None and "no notes and no chords" in info, "%s: an empty chart is refused: %s" % (a["id"], info))
                continue
            ok(built is not None, "%s: %s builds for %s: %s" % (path, a["id"], plat, info))
            b = parse(built)
            ok(emit(b) == built and not b["tail"], "%s: reads back whole" % a["id"])
        if built is None:
            print("  %-8s refused: %s" % (a["id"], info))
            continue
        f = facts(info)
        top = int(b["meta"]["maxNotes"])
        phr = j.get("phrases") or []
        want_levels = max([p.get("max_difficulty", 0) for p in phr], default=0) + 1
        ok(len(b["levels"]) == want_levels, "%s: %d levels for max difficulty %d" % (a["id"], len(b["levels"]), want_levels - 1))
        ok(top == len(j["notes"]) + len(j["chords"]), "%s: notes at the top (%d) are the flat lists' (%d notes, %d chords)" %
           (a["id"], top, len(j["notes"]), len(j["chords"])))
        if "event_count" in a:
            ok(top == int(a["event_count"]), "%s: the manifest's event_count" % a["id"])
        strings = sum(len(c["notes"]) for c in j["chords"]) + len(j["notes"])
        if "note_count" in a:
            ok(strings == int(a["note_count"]), "%s: the manifest's note_count (%s) is every string played (%d)" % (a["id"], a["note_count"], strings))
        ok(len(b["pis"]) == (len(phr) or len(b["pis"])), "%s: an iteration a phrase" % a["id"])
        for lv in b["levels"]:
            want_n = sum(len(l["notes"]) + len(l["chords"]) for p in phr for l in p["levels"] if l["difficulty"] == lv["difficulty"]
                         and l["difficulty"] <= p["max_difficulty"]) if phr else top
            ok(len(lv["notes"]) == want_n, "%s: level %d has %d notes, the JSON %d" % (a["id"], lv["difficulty"], len(lv["notes"]), want_n))
            times = [n["time"] for n in lv["notes"]]
            ok(times == sorted(times), "%s: notes in order" % a["id"])
            ok(all(0 <= n["anchorFret"] <= 24 and n["anchorWidth"] > 0 for n in lv["notes"]), "%s: every note has an anchor" % a["id"])
            ok(all(n["chordId"] < len(b["templates"]) and n["chordNoteId"] < len(b["chordNotes"]) for n in lv["notes"]), "%s: ids in range" % a["id"])
        ok(abs(b["meta"]["songLength"] - dur) < 0.01, "%s: the song length is the manifest's duration" % a["id"])
        ok(b["meta"]["tuning"] == [{"v": v} for v in (j["tuning"] + [0] * 6)[:6]], "%s: tuning" % a["id"])
        ok(len(b["beats"]) > 10 and len(b["sections"]) >= 1, "%s: beats and sections" % a["id"])
        print("  %-8s %d levels, %d iterations, %d sections, %d notes + %d chords at the top (manifest: event_count %s, note_count %s), "
              "%d chord notes records, %d templates, tuning %s, %s; repairs: %s" %
              (a["id"], len(b["levels"]), len(b["pis"]), len(b["sections"]), int(f["notes"][0].split()[0]), int(f["notes"][0].split()[2]),
               a.get("event_count", "-"), a.get("note_count", "-"), len(b["chordNotes"]), len(b["templates"]),
               [t["v"] for t in b["meta"]["tuning"]], "bass" if f["bass"][0].startswith("1") else "guitar", f["repaired"][0] or "none"))


BASE = {"name": "Lead", "tuning": [0, 0, 0, 0, 0, 0], "capo": 0,
        "notes": [{"t": 1.0, "s": 0, "f": 3}, {"t": 1.5, "s": 1, "f": 5, "sus": 0.4, "bn": 1.0}, {"t": 2.0, "s": 2, "f": 0},
                  {"t": 6.0, "s": 5, "f": 12, "sl": 14, "sus": 0.5}],
        "chords": [{"t": 3.0, "id": 0, "notes": [{"s": 0, "f": 3}, {"s": 1, "f": 5}]}, {"t": 3.5, "id": 0, "notes": []}],
        "anchors": [{"time": 1.0, "fret": 3, "width": 4}, {"time": 6.0, "fret": 12, "width": 4}],
        "handshapes": [{"chord_id": 0, "start_time": 3.0, "end_time": 4.0}],
        "templates": [{"name": "G5", "frets": [3, 5, -1, -1, -1, -1], "fingers": [1, 3, -1, -1, -1, -1]}]}


def vary(**kw):
    d = json.loads(json.dumps(BASE))
    d.update(kw)
    return d


def made_up_test(tool):
    def built(arr, timeline=None, length=0, bass=-1):
        p, info = tool.build("mac", arr, timeline, length, bass)
        ok(p is not None, "a chart that must build: %s" % info)
        s = parse(p)
        ok(emit(s) == p, "reads back whole")
        return s, facts(info)

    def refused(arr, word, timeline=None):
        p, info = tool.build("pc", arr, timeline)
        ok(p is None and word in info, "must be refused with '%s': %s" % (word, info if p is None else "built"))
        return info

    rep = lambda f: dict(x.rsplit("=", 1) for x in re.findall(r"[a-z ]+=\d+", f["repairs"][0]))
    count = lambda f, name: int({k.strip(): v for k, v in rep(f).items()}.get(name, 0))

    # the plain chart: one level, iterations made from nothing, beats made, one section
    s, f = built(BASE)
    ok([cs(p["name"]) for p in s["phrases"]] == ["COUNT", "p1"] and s["meta"]["songLength"] == 6.5, "no length given: to the last note's end")
    s, f = built(BASE, None, 10)
    ok(f["repairs"][0] == "0" and f["repaired"][0] == "", "a clean chart needs no repair: %s" % f["repairs"])
    ok(len(s["levels"]) == 1 and len(s["levels"][0]["notes"]) == 6, "one level, four notes and two chords")
    ok([cs(p["name"]) for p in s["phrases"]] == ["COUNT", "p1", "END"], "COUNT, the notes, END: %s" % [cs(p["name"]) for p in s["phrases"]])
    ok(len(s["sections"]) == 1 and len(s["beats"]) > 8 and s["beats"][0]["mask"] & 1, "a section and beats are made")
    ok(f["made"] == ["beats section iterations"], "and said to be: %s" % f["made"])
    ok(s["meta"]["maxNotes"] == 6 and abs(s["meta"]["pointsPerNote"] - 100000 / 6) < 1e-6, "score")
    notes = s["levels"][0]["notes"]
    ok([n["chordId"] for n in notes] == [-1, -1, -1, 0, 0, -1], "notes and chords in the order of their times")
    ok(notes[4]["mask"] & M_DOUBLESTOP and not notes[4]["mask"] & M_PANEL and notes[3]["mask"] & M_PANEL, "a chord with no notes: its template's")
    ok(notes[1]["mask"] & M_BEND and len(notes[1]["bends"]) == 1 and notes[1]["maxBend"] == 1.0, "a bend given as its peak gets a curve")
    ok(notes[2]["mask"] & M_OPEN and notes[0]["flags"] == 1 and notes[2]["flags"] == 0, "open string; fret numbers")
    ok(len(s["levels"][0]["anchorExts"]) == 1 and s["levels"][0]["anchorExts"][0]["fret"] == 14, "a slide's end")
    ok(s["templates"][0]["notes"] == [43, 50, -1, -1, -1, -1], "a guitar template's pitches")
    sb, fb = built(BASE, bass=1)
    ok(sb["templates"][0]["notes"] == [31, 38, -1, -1, -1, -1] and fb["bass"][0].startswith("1"), "a bass template's pitches")
    ok(f["notes"][0].startswith("4 chords 2 "), "facts: %s" % f["notes"])

    # a song timeline wins over the arrangement's own beats and sections; both forms of beat numbering
    tl = {"beats": [{"time": 0.5 * i, "measure": i // 4 + 1} for i in range(20)], "sections": [{"name": "intro", "number": 1, "time": 1.0},
                                                                                              {"name": "noguitar", "number": 1, "time": 7.0}]}
    s, f = built(vary(beats=[{"time": 0, "measure": 1}], sections=[{"name": "x", "number": 1, "time": 0}]), tl, 10)
    ok(len(s["beats"]) == 20 and [cs(x["name"]) for x in s["sections"]] == ["intro", "noguitar"], "the timeline's beats and sections")
    ok(f["made"] == ["iterations"], "only the iterations are made: %s" % f["made"])
    ok([(b["measure"], b["beat"]) for b in s["beats"][:6]] == [(1, 0), (1, 1), (1, 2), (1, 3), (2, 0), (2, 1)], "a measure number on every beat")
    tl2 = {"beats": [{"time": 0.5 * i, "measure": i // 4 + 1 if i % 4 == 0 else -1} for i in range(20)]}
    s2, _ = built(BASE, tl2, 10)
    ok([(b["measure"], b["beat"], b["mask"]) for b in s2["beats"]] == [(b["measure"], b["beat"], b["mask"]) for b in s["beats"]], "or -1 off the downbeat")
    ok([cs(p["name"]) for p in s["phrases"]] == ["COUNT", "p1", "END"] and s["pis"][2]["time"] == 7.0, "iterations at the sections")
    ok(s["meta"]["songLength"] == 10.0 and s["pis"][-1]["endTime"] == 10.0, "the given song length")
    s, _ = built(BASE, b"{ not json", 10)
    _, f = tool.build("mac", BASE, b"{ not json", 10)
    ok(count(facts(f), "timeline ignored") == 1, "a timeline that is not JSON is ignored and counted")

    # JSONC
    s, _ = built(b'{ // a hand-edited chart\n "notes": [ {"t": 1, "s": 0, "f": 1,}, /* more */ ], }')
    ok(len(s["levels"][0]["notes"]) == 1 and len(s["meta"]["tuning"]) == 6, "comments and trailing commas")

    # every repair
    s, f = built(vary(notes=BASE["notes"] + [{"t": 2.5, "s": 99, "f": 1}, {"t": -1, "s": 0, "f": 1}, {"t": "x", "s": 0, "f": 1}, {"s": 0, "f": 1},
                                             {"t": "1e999", "s": 0, "f": 1}, 7, {"t": 2.6, "s": 0, "f": -3}, {"t": 2.7, "s": 0}]))
    ok(count(f, "notes dropped") == 8 and len(s["levels"][0]["notes"]) == 6, "unusable notes are dropped: %s" % f["repairs"])
    s, f = built(vary(tuning=[0, 0, 0, 0], notes=[{"t": 1, "s": 3, "f": 1}, {"t": 2, "s": 4, "f": 1}], chords=[], handshapes=[]))
    ok(count(f, "notes dropped") == 1 and f["bass"][0].startswith("1"), "a string the tuning has not; four strings are a bass")
    s, f = built(vary(notes=[{"t": 1, "s": 0, "f": 500}, {"t": 2, "s": 0, "f": 30, "sl": 99}]))
    ok(count(f, "frets clamped") == 3 and [n["fret"] for n in s["levels"][0]["notes"] if n["chordId"] == -1] == [24, 24], "frets above 24 are 24")
    s, f = built(vary(notes=[{"t": "1.25", "s": "2", "f": "7", "sus": "0.5", "ho": "true", "pm": 1, "vb": "0"}]))
    n = s["levels"][0]["notes"][0]
    ok((n["time"], n["string"], n["fret"], n["sustain"]) == (1.25, 2, 7, 0.5) and n["mask"] & M_HAMMER and n["mask"] & M_PALMMUTE and
       not n["mask"] & M_VIBRATO and count(f, "values defaulted") == 0, "numbers and booleans as strings")
    s, f = built(vary(notes=[{"t": 1, "s": 0, "f": 3, "sus": [], "sl": {}, "ho": "maybe", "bn": "much", "bnv": 4}], capo="x", anchors="none"))
    n = s["levels"][0]["notes"][0]
    ok(n["sustain"] == 0 and n["slideTo"] == -1 and not n["mask"] & M_HAMMER and count(f, "values defaulted") == 7, "wrong kinds: %s" % f["repairs"])
    ok(count(f, "anchor lists made") == 1, "and anchors made for the list that was none")
    s, f = built(vary(chords=[{"t": 3.0, "id": 7, "notes": [{"s": 1, "f": 2}, {"s": 2, "f": 2}, {"s": 2, "f": 9}]},
                              {"t": 4.0, "id": 7, "notes": [{"s": 1, "f": 2}, {"s": 2, "f": 2}]}, {"t": 5.0, "id": 9}, {"id": 0},
                              {"t": 5.5, "id": 0, "notes": [{"s": 9, "f": 1}]}], handshapes=[{"chord_id": 44, "start_time": 3}]))
    ok(count(f, "templates made") == 1 and len(s["templates"]) == 2 and s["templates"][1]["frets"] == [-1, 2, 2, -1, -1, -1],
       "a chord naming a missing template gets one made from its notes, once: %s" % f["repairs"])
    ok(count(f, "chords dropped") == 2 and count(f, "notes dropped") == 2, "chords with nothing usable are dropped: %s" % f["repairs"])
    ok(count(f, "items skipped") == 1 and count(f, "hand shape lists made") == 1, "a hand shape naming a missing template; shapes made")
    ch = [n for n in s["levels"][0]["notes"] if n["chordId"] != -1]
    ok([c["chordId"] for c in ch] == [1, 1, 0] and len(s["levels"][0]["handshapes"]) == 2 and ch[0]["fp"][0] == 0 and ch[1]["fp"][0] == 0, "made shapes hold the chords")
    s, f = built(vary(notes=list(reversed(BASE["notes"])), anchors=list(reversed(BASE["anchors"]))))
    ok(count(f, "lists sorted") == 2 and [n["time"] for n in s["levels"][0]["notes"]] == sorted(n["time"] for n in s["levels"][0]["notes"]), "sorted")
    s, f = built(vary(anchors=[], handshapes=[], chords=[], notes=[{"t": 1, "s": 0, "f": 3}, {"t": 2, "s": 0, "f": 5}, {"t": 3, "s": 0, "f": 9}, {"t": 4, "s": 0, "f": 0},
                                                      {"t": 5, "s": 0, "f": 10}, {"t": 6, "s": 0, "f": 2}]))
    ok([(a["time"], a["fret"]) for a in s["levels"][0]["anchors"]] == [(1.0, 3), (3.0, 9), (6.0, 2)], "an anchor a hand position: %s" %
       [(a["time"], a["fret"]) for a in s["levels"][0]["anchors"]])
    ok(count(f, "anchor lists made") == 1 and count(f, "hand shape lists made") == 0, "counted")
    s, f = built(vary(anchors=[{"time": 5.0, "fret": 12}]))
    ok(s["levels"][0]["anchors"][0]["time"] == 1.0 and all(n["anchorFret"] > 0 for n in s["levels"][0]["notes"]), "an anchor before the first note")
    ok("anchor" in f["made"][0], "and said to be made")
    s, f = built(vary(templates=[7, {"name": 5, "frets": "x"}], chords=[{"t": 3, "id": 1, "notes": [{"s": 0, "f": 1}, {"s": 1, "f": 1}]}], handshapes=[]))
    ok(len(s["templates"]) == 2 and count(f, "values defaulted") == 3, "templates that are none keep their place: %s" % f["repairs"])
    s, f = built(vary(sections=[{"name": "verse", "time": 2}, {"name": "x"}, {"name": "chorus", "time": 2}, {"name": 4, "time": 1}],
                      beats=[{"time": 1}, {"time": "zero"}, {"time": 0.5, "measure": 1}, {"time": 0.5}, {"time": 3, "measure": "2"}]))
    ok([cs(x["name"]) for x in s["sections"]] == ["riff", "verse"] and [b["time"] for b in s["beats"]] == [0.5, 1.0, 3.0], "sections and beats repaired")
    ok(s["beats"][2]["measure"] == 2 and s["beats"][2]["beat"] == 0, "a measure number as a string")
    s, f = built(vary(tones={"base": "clean", "changes": [{"t": 4, "name": "lead"}, {"t": 2, "name": "clean"}, {"name": "x"}, {"t": 5, "name": "a"},
                                                         {"t": 6, "name": "b"}, {"t": 7, "name": "c"}, {"t": 8, "name": "lead"}]}))
    ok([(x["time"], x["id"]) for x in s["tones"]] == [(2.0, 0), (4.0, 1), (5.0, 2), (6.0, 3), (8.0, 1)] and f["tone"] == ["0 clean", "1 lead", "2 a", "3 b"],
       "tones: four names at most: %s" % s["tones"])
    s, f = built(vary(tones=[1, 2], phrases="none", stats={"x": 1}, extra=[1, 2, 3]))
    ok(len(s["levels"]) == 1 and count(f, "values defaulted") == 1, "unknown keys are ignored; phrases of the wrong kind are none")

    # phrases: a ladder with holes
    lv = lambda d, ts: {"difficulty": d, "notes": [{"t": t, "s": 0, "f": 3} for t in ts], "anchors": [{"time": ts[0], "fret": 3}] if ts else []}
    ph = [{"start_time": 0, "end_time": 2, "max_difficulty": 0, "levels": [lv(0, [])]},
          {"start_time": 2, "end_time": 4, "max_difficulty": 2, "levels": [lv(2, [2, 2.5, 3, 3.5]), lv(0, [2])]},
          {"start_time": 4, "end_time": 6, "max_difficulty": 1, "levels": [lv(0, [4]), lv(1, [4, 5]), lv(5, [4, 4.1, 4.2])]},
          {"start_time": 4, "end_time": 6, "max_difficulty": 1, "levels": [lv(0, [4.5])]},
          {"start_time": "x", "levels": []}, {"start_time": 8, "end_time": 9, "max_difficulty": 99, "levels": [lv(0, []), lv(77, [8]), lv(0, [8])]}]
    s, f = built(vary(phrases=ph), None, 10)
    ok(len(s["levels"]) == 3 and [len(l["notes"]) for l in s["levels"]] == [2, 3, 4], "levels of a ladder with holes: %s" % [len(l["notes"]) for l in s["levels"]])
    ok([p["maxDifficulty"] for p in s["phrases"]] == [0, 2, 1, 0] and [cs(p["name"]) for p in s["phrases"]] == ["COUNT", "p1", "p2", "END"], "phrases")
    ok(s["meta"]["maxNotes"] == 6 and [x["v"] for x in s["levels"][1]["notesInIter"]] == [0, 1, 2, 0], "a missing level plays the one below")
    ok(count(f, "items skipped") == 4, "a phrase at another's time, one without a time, levels out of range or twice: %s" % f["repairs"])
    ok(all(n["pi"] == i for i, pi in enumerate(s["pis"]) for l in s["levels"] for n in l["notes"] if pi["time"] <= n["time"] < pi["endTime"]), "iterations")

    # what is refused
    refused(b"", "empty")
    refused(b"[1, 2", "not JSON")
    refused(b"\x00\xff\xfe garbage", "not JSON")
    refused(b"[]", "not a JSON object")
    refused(json.dumps(BASE).encode()[:200], "not JSON")
    refused(vary(notes=[], chords=[]), "no notes and no chords")
    refused({}, "no notes and no chords")
    refused(vary(notes=[{"t": float("nan"), "s": 0, "f": 1}], chords=[]), "not JSON")  # NaN is not JSON at all
    refused(vary(notes=[{"t": -5, "s": 0, "f": 1}, {"t": 1, "s": 99, "f": 1}], chords=[]), "2 dropped")
    refused(vary(tuning=[0] * 7), "7 strings")
    refused(vary(tuning=[0] * 8), "8 strings")
    many = [{"t": i * 0.001, "s": i % 6, "f": i % 20} for i in range(1000000)]
    info = refused(vary(notes=many), "more than the 32767")
    s, f = built(vary(notes=many[:32000], chords=[], handshapes=[]))
    ok(len(s["levels"][0]["notes"]) == 32000 and s["levels"][0]["notes"][31999]["prev"] == 31998, "32000 notes still fit")
    deep = [{"start_time": i, "max_difficulty": 0, "levels": [lv(0, [i + 0.5] * 20000)]} for i in range(2)]
    refused(vary(phrases=deep), "more than the 32767")
    print("made-up charts: repairs, limits and refusals as documented (a million notes: %s)" % info)


def main():
    global verbose
    ap = argparse.ArgumentParser()
    ap.add_argument("sngtest")
    ap.add_argument("--tools")
    ap.add_argument("--samples")
    ap.add_argument("--feedpak", action="append", default=[])
    ap.add_argument("--fuzz", type=int, default=1500)
    ap.add_argument("--verbose", action="store_true")
    a = ap.parse_args()
    verbose = a.verbose
    with tempfile.TemporaryDirectory() as tmp:
        tool = Tool(a.sngtest, tmp)
        made_up_test(tool)
        if a.samples:
            ok(a.tools, "--samples needs --tools")
            ok(samples_test(tool, a.tools, a.samples) > 0, "song files in the samples")
        for p in a.feedpak:
            feedpak_test(tool, p)
        # fuzz: the made-up chart with a ladder, and each real arrangement
        seeds = [json.dumps(vary(phrases=[{"start_time": 0, "end_time": 9, "max_difficulty": 1, "levels": [
            dict(BASE, difficulty=0), dict(BASE, difficulty=1)]}], beats=[{"time": 0.5 * i, "measure": i // 4} for i in range(12)],
            sections=[{"name": "intro", "number": 1, "time": 1}], tones={"base": "a", "changes": [{"t": 2, "name": "b"}]})).encode()]
        rounds = [a.fuzz * 4]
        for p in a.feedpak:
            z = zipfile.ZipFile(p)
            for n in z.namelist():
                if n.startswith("arrangements/") and n.endswith(".json"):
                    seeds.append(z.read(n))
                    rounds.append(max(a.fuzz // 50, 4))
        for i, (seed, n) in enumerate(zip(seeds, rounds)):
            src = tool.path("fuzz.json")
            open(src, "wb").write(seed)
            r = tool.run("fuzz", src, str(n), str(1234567 + i))
            ok(r.stdout.startswith("fuzz: "), "fuzz ran")
            print("%s (seed file of %d bytes)" % (r.stdout.strip(), len(seed)))
    print("ok, %d checks" % checks)


if __name__ == "__main__":
    main()
