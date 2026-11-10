#!/usr/bin/env python3
"""The song texts' test: tests/run_songtext_test.py SONGTEXTTEST [--tools DIR --samples DIR]

songtexttest (tests/songtexttest.c) runs its own checks, then makes the texts of songs described here: titles that
need escaping, songs with next to nothing known of them, arrangements whose names collide, twenty arrangements.
Every manifest must parse as JSON and hold what was given, every XML text must be well formed, the aggregate graph
must tell of exactly the archive's files, and the PC's texts must differ from the Mac's as src/psarc.c says they do.

With --samples, every .psarc in that folder is read with rs-rearmed's reader (tools/psarc_extract.py in --tools'
DIR, given this repository's keys, as tests/run_varc_test.py does) and its own manifests are turned into a song
description; the texts made of that are compared with the archive's: the manifests and the .hsan value by value,
the xblock entity by entity, the graph node by node. What may differ is listed in KNOWN, each with its reason; any
other difference fails. The differences found are counted at the end."""
import argparse, collections, json, os, re, struct, subprocess, sys, tempfile
import xml.etree.ElementTree as ET

checks = 0
TEXT_KINDS = 8  # SB_E_APPID .. SB_E_MANIFEST
MAX_ARR = 256   # SB_MAX_ARR


def ok(cond, what):
    global checks
    checks += 1
    if not cond:
        sys.exit("FAIL: " + what)


# --- songtexttest ----------------------------------------------------------------------------------------------------
class Made:
    """What `songtexttest gen` made of a song description: the entries' paths and texts per platform, the names"""

    def __init__(self, binary, spec, tmp, tag):
        self.dir = os.path.join(tmp, tag)
        p = os.path.join(tmp, tag + ".json")
        json.dump(spec, open(p, "w"))
        r = subprocess.run([binary, "gen", p, self.dir], capture_output=True, text=True)
        self.code, self.out, self.err = r.returncode, r.stdout, r.stderr
        self.paths = {"mac": [], "pc": []}  # [(kind, arr, size, path)]
        self.text = {"mac": {}, "pc": {}}   # path -> bytes
        self.arr, self.events, self.key, self.audio = [], [], None, None
        for line in r.stdout.splitlines():
            f = line.split("\t")
            if f[0] == "entry":
                self.paths[f[1]].append((int(f[2]), int(f[3]), int(f[4]), f[5]))
                if int(f[2]) < TEXT_KINDS:
                    data = open(os.path.join(self.dir, f[1], f[5]), "rb").read()
                    ok(len(data) == int(f[4]), "%s: %s is %d bytes, not the %s told" % (tag, f[5], len(data), f[4]))
                    self.text[f[1]][f[5]] = data
            elif f[0] == "arr":
                self.arr.append((f[2], f[3]))
            elif f[0] == "event":
                self.events.append((f[1], int(f[2])))
            elif f[0] == "key":
                self.key = f[1]
            elif f[0] == "audio":
                self.audio = (int(f[1]), int(f[2]))

    def one(self, platform, suffix):
        hits = [d for p, d in self.text[platform].items() if p.endswith(suffix)]
        ok(len(hits) == 1, "one entry ending in %s, not %d" % (suffix, len(hits)))
        return hits[0]


def gen(binary, spec, tmp, tag):
    m = Made(binary, spec, tmp, tag)
    ok(m.code == 0, "%s: songtexttest: %s%s" % (tag, m.out[-300:], m.err[-2000:]))
    return m


def attributes(data):
    """{persistent ID: attributes} of a manifest or .hsan, and the rest of the top level"""
    j = json.loads(data.decode("utf-8-sig"))
    ok(all(list(e) == ["Attributes"] for e in j["Entries"].values()), "an entry is its attributes")
    return {k: e["Attributes"] for k, e in j["Entries"].items()}, {k: v for k, v in j.items() if k != "Entries"}


def graph_nodes(data):
    """The aggregate graph by file: {relpath: {tags, canonical, name, logpath, llid, uuid}}"""
    by_id = collections.OrderedDict()
    text = data.decode("utf-8-sig")
    for line in text.replace("\r\n", "\n").split("\n"):
        if not line:
            continue
        m = re.fullmatch(r'<urn:uuid:([0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})> <http://emergent\.net/aweb/1\.0/(\w+)> "([^"]*)"\.', line)
        ok(m, "a line of the graph is no statement: %r" % line)
        by_id.setdefault(m.group(1), []).append((m.group(2), m.group(3)))
    nodes = {}
    for uuid, st in by_id.items():
        d = collections.defaultdict(list)
        for pred, v in st:
            d[pred].append(v)
        ok(set(d) <= {"tag", "canonical", "name", "relpath", "llid", "logpath"}, "a predicate the graph does not have: %s" % set(d))
        for pred in ("canonical", "name", "relpath"):
            ok(len(d[pred]) == 1, "node %s has %d %s" % (uuid, len(d[pred]), pred))
        ok(len(d["llid"]) == len(d["logpath"]) <= 1, "node %s: a link ID and a logical path go together" % uuid)
        ok(len(set(d["tag"])) == len(d["tag"]), "node %s has a tag twice" % uuid)
        ok(d["relpath"][0] not in nodes, "two nodes for " + d["relpath"][0])
        nodes[d["relpath"][0]] = {"tags": set(d["tag"]), "canonical": d["canonical"][0], "name": d["name"][0],
                                  "logpath": d["logpath"][0] if d["logpath"] else None, "llid": d["llid"][0] if d["llid"] else None, "uuid": uuid}
    return nodes


def as_pc(mac):
    """The Mac's aggregate graph as src/psarc.c (graph_to, drop_mac_sng_tags, paths_to) serves it to the PC game"""
    lines = mac.decode().split("\n")
    sng = {l.split(" ")[0] for l in lines if 'canonical> "/songs/bin/macos"' in l}
    lines = [l for l in lines if not (l.endswith('/tag> "macos".') and l.split(" ")[0] in sng)]
    return "\n".join(lines).replace("audio/mac", "audio/windows").replace("bin/macos", "bin/generic").replace("macos", "dx9").encode()


def xblock_entities(data):
    root = ET.fromstring(data.decode("utf-8-sig"))
    ok(root.tag == "game" and [c.tag for c in root] == ["entitySet"], "the xblock is a game with an entity set")
    out = []
    for e in root[0]:
        ok(e.tag == "entity" and [c.tag for c in e] == ["properties"], "an entity with properties")
        props = {}
        for p in e[0]:
            ok(p.tag == "property" and [c.tag for c in p] == ["set"] and p.get("name") not in props, "a property with one value")
            props[p.get("name")] = p[0].get("value")
        out.append((dict(e.attrib), props))
    return out


def fnv(name):
    h = 2166136261
    for c in name.lower().encode():
        h = (h * 16777619 & 0xffffffff) ^ c
    return h


def well_formed(m, tag, spec):
    """What must hold of any song's texts"""
    narr = min(len(spec["arr"]), MAX_ARR)
    ok(len(m.arr) == narr and len({n for n, _ in m.arr}) == narr and len({p for _, p in m.arr}) == narr, tag + ": arrangements with names and IDs of their own")
    ok(re.fullmatch(r"[A-Za-z0-9]{1,32}", m.key), tag + ": the key " + m.key)
    lkey = m.key.lower()
    for name, pid in m.arr:
        ok(re.fullmatch(r"[a-z0-9]{1,24}", name) and re.fullmatch(r"[0-9A-F]{32}", pid) and name != "showlights", "%s: arrangement %s %s" % (tag, name, pid))
    ok([e for e, _ in m.events] == ["Play_" + m.key, "Play_%s_Preview" % m.key] and all(fnv(e) == i for e, i in m.events), tag + ": the events and their Wwise IDs")
    for platform, audio, sng in (("mac", "audio/mac", "songs/bin/macos"), ("pc", "audio/windows", "songs/bin/generic")):
        paths = [p for _, _, _, p in m.paths[platform]]
        ok(len(paths) == len(set(paths)) == 14 + 2 * narr, "%s: %d entries, each its own path" % (tag, 14 + 2 * narr))
        ok(all(re.fullmatch(r"[a-z0-9_./]+", p) for p in paths), tag + ": paths in lower case")
        want = {"appid.appid", "flatmodels/rs/rsenumerable_root.flat", "flatmodels/rs/rsenumerable_song.flat", lkey + "_aggregategraph.nt",
                "gamexblocks/nsongs/%s.xblock" % lkey, "manifests/songs_dlc_%s/songs_dlc_%s.hsan" % (lkey, lkey), "songs/arr/%s_showlights.xml" % lkey,
                "%s/song_%s.bnk" % (audio, lkey), "%s/song_%s_preview.bnk" % (audio, lkey),
                "%s/%d.wem" % (audio, spec.get("audio_id") or m.audio[0]), "%s/%d.wem" % (audio, spec.get("preview_id") or m.audio[1])}
        want |= {"gfxassets/album_art/album_%s_%d.dds" % (lkey, n) for n in (64, 128, 256)}
        want |= {"manifests/songs_dlc_%s/%s_%s.json" % (lkey, lkey, n) for n, _ in m.arr} | {"%s/%s_%s.sng" % (sng, lkey, n) for n, _ in m.arr}
        ok(set(paths) == want, "%s: the %s entries are %s" % (tag, platform, sorted(set(paths) ^ want)))
        t = m.text[platform]
        ok(t["appid.appid"] == b"248750", tag + ": the AppID")

        # the manifests: JSON; the header is the manifests' own values
        head, top = attributes(m.one(platform, ".hsan"))
        ok(top == {"InsertRoot": "Static.Songs.Headers"} and list(head) == [p for _, p in m.arr], tag + ": the header has the arrangements in order")
        for name, pid in m.arr:
            a, top = attributes(t["manifests/songs_dlc_%s/%s_%s.json" % (lkey, lkey, name)])
            ok(list(a) == [pid] and top == {"InsertRoot": "Static.Songs.Entries", "IterationVersion": 3, "ModelName": "RSEnumerable_Song"}, tag + ": a manifest of one entry")
            a, h = a[pid], head[pid]
            ok(a["PersistentID"] == pid and a["SongKey"] == a["DLCKey"] == m.key and a["FullName"] == "%s_%s" % (m.key, name.capitalize()[:1] + name[1:]),
               tag + ": a manifest's names")
            ok(a["ManifestUrn"] == "urn:database:json-db:%s_%s" % (lkey, name) and a["SongEvent"] == "Play_" + m.key, tag + ": a manifest's URN and event")
            own = {"Representative", "RouteMask", "BassPick"}
            ok(all(a[k] == v for k, v in h.items() if k not in own) and set(h) - set(a) <= own, "%s: %s: the header's values are the manifest's" % (tag, name))
            if "Tones" in a:
                ok(len(a["Tones"]) >= 1 and all(isinstance(x, dict) for x in a["Tones"]), tag + ": tones")
                ok(h["Representative"] == a["ArrangementProperties"]["represent"] and h["RouteMask"] == a["ArrangementProperties"]["routeMask"], tag + ": the header's path")
                ok(len(a["ArrangementProperties"]) == 32 and set(a["Tuning"]) == {"string%d" % i for i in range(6)}, tag + ": properties and tuning")
        represented = collections.Counter(h["RouteMask"] for h in head.values() if h.get("Representative"))
        ok(all(v == 1 for v in represented.values()), tag + ": one arrangement represents a path")

        # the XML texts
        ents = xblock_entities(m.one(platform, ".xblock"))
        ok([(e["id"], e["name"].lower()) for e, _ in ents] == [(p.lower(), "%s_%s" % (lkey, n)) for n, p in m.arr], tag + ": an entity per arrangement")
        for (e, props), (name, _) in zip(ents, m.arr):
            ok(props == {"Header": "urn:database:hsan-db:songs_dlc_" + lkey, "Manifest": "urn:database:json-db:%s_%s" % (lkey, name),
                         "SngAsset": "urn:application:musicgame-song:%s_%s" % (lkey, name), "AlbumArtSmall": "urn:image:dds:album_%s_64" % lkey,
                         "AlbumArtMedium": "urn:image:dds:album_%s_128" % lkey, "AlbumArtLarge": "urn:image:dds:album_%s_256" % lkey, "LyricArt": "",
                         "ShowLightsXMLAsset": "urn:application:xml:%s_showlights" % lkey, "SoundBank": "urn:audio:wwise-sound-bank:song_" + lkey,
                         "PreviewSoundBank": "urn:audio:wwise-sound-bank:song_%s_preview" % lkey} and e["modelName"] == "RSEnumerable_Song" and e["iterations"] == "0",
               tag + ": an entity's properties")
        lights = ET.fromstring(m.one(platform, "_showlights.xml").decode())
        times = [float(l.get("time")) for l in lights]
        ok(lights.tag == "showlights" and int(lights.get("count")) == len(lights) >= 3 and all(l.tag == "showlight" and set(l.attrib) == {"time", "note"} for l in lights),
           tag + ": showlights and their count")
        ok(times == sorted(times) and times[0] >= 0 and all(re.fullmatch(r"\d+\.\d{3}", l.get("time")) for l in lights), tag + ": the lights in order of time")
        notes = [int(l.get("note")) for l in lights]
        ok(all(24 <= n <= 35 or 48 <= n <= 59 or n == 66 for n in notes) and 24 <= notes[0] <= 35 and 48 <= notes[1] <= 59, tag + ": fog and beam notes")
        for f in ("root", "song"):
            ok(ET.fromstring(t["flatmodels/rs/rsenumerable_%s.flat" % f].decode("utf-8-sig")).get("name").lower() == "rsenumerable_" + f, tag + ": a flat model")

        # the graph: a node for each file the game finds by URN, and for no other
        nodes = graph_nodes(m.one(platform, "_aggregategraph.nt"))
        in_graph = {p for p in paths if not p.endswith((".appid", ".flat", ".nt", ".wem"))}
        ok(set(nodes) == {"/" + p for p in in_graph}, "%s: the graph's files are %s" % (tag, sorted(set(nodes) ^ {"/" + p for p in in_graph})))
        ok(len({n["llid"] for n in nodes.values() if n["llid"]}) == sum(1 for n in nodes.values() if n["llid"]), tag + ": link IDs of their own")
        tags = {".json": {"database", "json-db"}, ".hsan": {"database", "hsan-db"}, ".xblock": {"emergent-world", "x-world"}, ".xml": {"application", "xml"},
                ".dds": {"dds", "image"}, ".sng": {"application", "musicgame-song"} | ({"macos"} if platform == "mac" else set()),
                ".bnk": {"audio", "wwise-sound-bank", "macos" if platform == "mac" else "dx9"}}
        for rel, n in nodes.items():
            ext = os.path.splitext(rel)[1]
            log = {".sng": "/songs/bin/" + os.path.basename(rel), ".bnk": "/audio/" + os.path.basename(rel), ".xml": rel, ".dds": rel}.get(ext)
            ok(n["tags"] == tags[ext] and n["canonical"] == os.path.dirname(rel) and n["name"] == os.path.basename(rel)[:-len(ext)] and n["logpath"] == log,
               "%s: the graph's node of %s: %s" % (tag, rel, n))
    ok(as_pc(m.one("mac", "_aggregategraph.nt")) == m.one("pc", "_aggregategraph.nt"), tag + ": the PC's graph is the Mac's as psarc.c turns it")
    for k in ("flatmodels/rs/rsenumerable_root.flat", "flatmodels/rs/rsenumerable_song.flat", "appid.appid"):
        ok(m.text["mac"][k] == m.text["pc"][k], tag + ": %s is the same on both platforms" % k)
    for suffix in (".hsan", ".xblock", "_showlights.xml"):
        ok(m.one("mac", suffix) == m.one("pc", suffix), tag + ": %s is the same on both platforms" % suffix)


def made_up(binary, tmp):
    key = lambda artist, title: subprocess.run([binary, "key", artist, title], capture_output=True, text=True, check=True).stdout.strip()
    # song keys
    songs = [("ABBA", "Dancing Queen"), ("AC/DC", "Back In Black"), ("ACDC", "Back In Black"), ("Motörhead", 'Été — "Live"'), ("", ""), ("日本", "歌"),
             ("x" * 300, "y" * 300), ("x" * 300, "y" * 299 + "z"), ("3 Doors Down", "Kryptonite"), ("a\\b", "c\nd"), ("Queen", "Innuendo"), ("Queen", "Innuendo (Live)")]
    songs += [("Artist %d" % i, "Song %d" % j) for i in range(40) for j in range(40)] + [("The Same Long Artist Name Here", "Track %d" % i) for i in range(400)]
    keys = [key(a, t) for a, t in songs]
    ok(all(re.fullmatch(r"[A-Za-z][A-Za-z0-9]{8,29}", k) for k in keys), "song keys are a letter, then letters and digits, 9 to 30 in all")
    ok(len(set(keys)) == len(keys) and len({k.lower() for k in keys}) == len(keys), "%d songs, %d keys" % (len(keys), len(set(keys))))
    ok(keys == [key(a, t) for a, t in songs[:12]] + keys[12:], "the same song has the same key in another process")
    ok(keys[0].startswith("ABBADancingQueen") and keys[1].startswith("ACDCBackInBlack") and keys[1][:15] == keys[2][:15] and keys[8].startswith("S3Doors"), "what a key is made of")
    print("song keys: %d songs, all different (e.g. %s, %s, %s)" % (len(keys), keys[0], keys[3], keys[5]))

    # titles that need escaping
    lead = lambda **k: dict({"type": 0, "tuning": [0] * 6, "notes": [10, 20, 40], "sections": [["intro", 1.5], ["verse", 20.0], ["solo", 31.25]],
                             "phrases": [["p", 3]], "iters": [[0, 1.5], [0, 20.0]]}, **k)
    titles = ["AC/DC", 'Été — "Live"', "Back\\slash and a\nnewline", "T" * 300, "tab\tcontrol\x01\x1f\x7f end", "日本語のタイトル 🎸", "</xml> & <tags> ' \"", "", "\\", '"']
    for i, title in enumerate(titles):
        spec = {"key": "Esc%d" % i, "title": title, "artist": title, "album": title, "year": 1999, "length": 60.5, "arr": [lead(), {"type": 4}]}
        m = gen(binary, spec, tmp, "esc%d" % i)
        well_formed(m, "title %d" % i, spec)
        a = list(attributes(m.one("mac", "_lead.json"))[0].values())[0]
        shown = title or None
        ok(a["SongName"] == (shown or "Unknown Title") and a["ArtistName"] == (shown or "Unknown Artist") and a["AlbumName"] == (shown or "Unknown Album"),
           "title %d comes back from the manifest: %r" % (i, a["SongName"]))
        h = list(attributes(m.one("mac", ".hsan"))[0].values())[0]
        ok(h["SongName"] == a["SongName"] and a["SongYear"] == 1999 and a["SongLength"] == 60.5, "title %d in the header" % i)
        ok(m.one("mac", "_lead.json").decode("utf-8"), "valid UTF-8")
    print("%d titles that need escaping come back from the manifests as they went in" % len(titles))

    # no arrangements: the one refusal
    for spec in ({"key": "None", "title": "t", "length": 5, "arr": []}, {"arr": []}):
        m = Made(binary, spec, tmp, "none")
        ok(m.code == 2 and m.out.startswith("error\t") and not m.paths["mac"], "a song without arrangements is refused: %s%s" % (m.out, m.err))

    # twenty arrangements, and more than are used
    spec = {"key": "Twenty", "title": "Twenty", "length": 100, "arr": [lead(type=i % 4) for i in range(18)] + [{"type": 4}, {"type": 4}]}
    m = gen(binary, spec, tmp, "twenty")
    well_formed(m, "twenty", spec)
    ok([n for n, _ in m.arr][:9] == ["lead", "rhythm", "combo", "bass", "lead2", "rhythm2", "combo2", "bass2", "lead3"] and m.arr[19][0] == "vocals2", "twenty arrangements' names")
    spec = {"key": "Lots", "length": 100, "arr": [{"type": i % 5} for i in range(MAX_ARR + 44)]}
    m = gen(binary, spec, tmp, "lots")
    well_formed(m, "lots", spec)
    print("20 arrangements: %d entries; %d arrangements: the first %d used, %d entries" % (len(gen(binary, {"key": "Twenty", "arr": [{}] * 20}, tmp, "t20").paths["mac"]),
                                                                                        MAX_ARR + 44, len(m.arr), len(m.paths["mac"])))

    # a package as it comes: five arrangements by id, two of them leads, one vocals; tones as the game's own objects
    tone = lambda k: json.dumps({"Key": k, "Name": k, "GearList": {"Amp": {"Key": "Amp_Whatever", "KnobValues": {"x": 1.5}}}, "Volume": "-18.0", "Unknown": [1, {"a": None}]})
    spec = {"title": "Clean My Wounds", "artist": "Corrosion Of Conformity", "length": 210.0,
            "arr": [lead(type=3, name="bass"), lead(name="lead", tones=[tone("Dirt"), tone("Clean")], tone_a="Dirt", tone_b="Clean"),
                    lead(name="lead-2", tones=[tone("Dirt")]), lead(type=1, name="rhythm"), {"type": 4, "name": "vocals"}]}
    m = gen(binary, spec, tmp, "five")
    well_formed(m, "five", spec)
    ok([n for n, _ in m.arr] == ["bass", "lead", "lead2", "rhythm", "vocals"] and m.key == key(spec["artist"], spec["title"]), "five arrangements by their ids")
    att = lambda n: list(attributes(m.one("mac", "_%s.json" % n))[0].values())[0]
    a, a2, v = att("lead"), att("lead2"), att("vocals")
    ok(a["Tone_Base"] == "Dirt" and a["Tone_A"] == "Dirt" and a["Tone_B"] == "Clean" and a["Tone_C"] == a["Tone_D"] == a["Tone_Multiplayer"] == "", "tone names")
    ok(a["Tones"] == [json.loads(tone("Dirt")), json.loads(tone("Clean"))] and a2["Tones"] == [json.loads(tone("Dirt"))], "tones pass through whole")
    ok(a["ArrangementProperties"]["represent"] == 1 and a2["ArrangementProperties"]["represent"] == 0 and a2["ArrangementProperties"]["bonusArr"] == 0 and
       a2["ArrangementName"] == "Lead" and a2["ArrangementProperties"]["routeMask"] == 1, "the second lead is a lead that does not represent its path")
    ok(att("bass")["Tones"][0]["GearList"]["Amp"]["Key"] == "Bass_Amp_LT25B" and att("rhythm")["Tones"][0]["GearList"]["Cabinet"]["Key"] == "Cab_TW112C_57_Edge",
       "arrangements without tones get the default one of their kind")
    ok(set(v) == VOCALS_JSON and v["ArrangementName"] == "Vocals", "the vocals' manifest has its own few attributes: %s" % sorted(set(v) ^ VOCALS_JSON))
    hv = attributes(m.one("mac", ".hsan"))[0][m.arr[4][1]]
    ok(set(hv) == VOCALS_HSAN, "the vocals' header: %s" % sorted(set(hv) ^ VOCALS_HSAN))
    ok(a["Sections"][2]["IsSolo"] and a["Sections"][0]["UIName"] == "$[34286] Intro [1]" and a["Sections"][2]["EndTime"] == 210.0, "sections")
    m2 = gen(binary, spec, tmp, "five-again")
    ok(m2.text == m.text and m2.arr == m.arr and m2.paths == m.paths, "the same song gives the same texts in another process")
    print("five arrangements by id (%s): key %s, persistent IDs %s..." % (", ".join(n for n, _ in m.arr), m.key, m.arr[1][1][:8]))

    # songs nothing is known of, and names that cannot be entry names
    odd = [{"arr": [{}]},
           {"key": "", "title": "", "artist": "", "album": "", "length": 0, "year": 0, "arr": [{"name": ""}, {"name": ""}, {"name": None}]},
           {"key": "../../etc/passwd", "title": None, "length": -5, "arr": [{"name": "../x"}, {"name": "a/b\\c"}, {"name": "LEAD"}, {"name": "lead"}, {"name": "Lead "}]},
           {"key": "ключ", "title": "Песня", "artist": "Группа", "arr": [{"name": "соло", "type": 99}, {"name": "бас", "type": 3}, {"name": "🎸", "type": -1}]},
           {"key": "K" * 500, "title": "T" * 5000, "arr": [{"name": "n" * 500, "pid": "zz"}, {"name": "n" * 500, "pid": "0" * 32}, {"name": "n" * 400, "pid": "0" * 32}]},
           {"title": "x", "arr": [{"name": "showlights"}, {"name": "vocals", "type": 0}, {"type": 4}, {"name": "preview"}, {"name": "64"}]},
           {"title": "y", "length": 1e300, "year": -1, "audio_id": 4294967295, "preview_id": 1, "lights": [5, 3, -1, 1e9, 7, 7, 8],
            "arr": [lead(tones=["", "[]", "{", tone("Ok"), "null"], path=77, capo=-3, tempo=-1, sections=[["", -5], ["Verse 2", 3], ["weird one", 2]],
                         iters=[[9, 0], [0, 1]], chords=[[3, 1, [1]], [2, 0, [5]], [3, 1, [2]], [3, 2, []]])]}]
    for i, spec in enumerate(odd):
        m = gen(binary, spec, tmp, "odd%d" % i)
        well_formed(m, "odd %d" % i, spec)
        m2 = gen(binary, spec, tmp, "odd%d-again" % i)
        ok(m2.text == m.text, "odd %d: the same texts again" % i)
    m = gen(binary, odd[2], tmp, "odd2")
    ok(m.key == "etcpasswd" and [n for n, _ in m.arr] == ["x", "abc", "lead", "lead2", "lead3"], "names made of what is left: %s %s" % (m.key, m.arr))
    m = gen(binary, odd[4], tmp, "odd4")
    ok(m.key == "K" * 32 and [n for n, _ in m.arr] == ["n" * 19, "n" * 19 + "2", "n" * 19 + "3"] and m.arr[1][1] == "0" * 32 and m.arr[2][1] != "0" * 32,
       "long names are cut and numbered apart; a persistent ID is used once")
    m = gen(binary, odd[6], tmp, "odd6")
    a = list(attributes(m.one("mac", "_lead.json"))[0].values())[0]
    ok([t["Key"] for t in a["Tones"]] == ["Ok"] and a["Tone_Base"] == "Ok" and "CapoFret" not in a and a["SongAverageTempo"] == 120 and
       a["Chords"] == {"3": {"1": [1], "2": []}} and a["ArrangementProperties"]["routeMask"] == 1 and a["PhraseIterations"][0]["PhraseIndex"] == 0,
       "what cannot be is left out or replaced: %s" % a["Chords"])
    lights = [l.get("time") for l in ET.fromstring(m.one("mac", "_showlights.xml").decode())]
    ok(lights == ["5.000", "5.000", "5.000", "7.000", "7.000", "7.000", "7.000", "8.000", "8.000"], "light times out of order are left out: %s" % lights)
    print("%d songs with missing, empty, long and impossible values: all made, the same twice" % len(odd))


VOCALS_JSON = {"AlbumArt", "ArrangementName", "ArrangementSort", "BlockAsset", "DLC", "DLCKey", "DynamicVisualDensity", "FullName", "InputEvent",
               "LeaderboardChallengeRating", "ManifestUrn", "MasterID_PS3", "MasterID_RDV", "MasterID_XBox360", "PersistentID", "PreviewBankPath",
               "RelativeDifficulty", "SKU", "Shipping", "ShowlightsXML", "SongAsset", "SongBank", "SongEvent", "SongKey", "SongXml"}
VOCALS_HSAN = {"AlbumArt", "ArrangementName", "DLC", "DLCKey", "LeaderboardChallengeRating", "ManifestUrn", "MasterID_RDV", "PersistentID", "SKU", "Shipping", "SongKey"}

# --- the sample archives ---------------------------------------------------------------------------------------------
# What may differ between a real archive's texts and ours made of its own facts: (which archives, which text, where,
# how) and why. "how": "real" = only the real one has it, "ours" = only ours, "value" = both, not the same.
# Archives: "builder" was made by DLC Builder (its xblock has no byte order mark), "toolkit" by the Rocksmith
# Toolkit, "dlc" is the game's own. Any other difference fails the test.
KNOWN = [
    ("toolkit builder", "json", r"IterationVersion", "value", "the DLC has 3, custom songs' tools write 2"),
    ("builder", "json hsan", r"CapoFret", "real", "DLC Builder writes CapoFret 0.0 without a capo; the DLC and the Toolkit do not, nor do we"),
    ("builder", "json", r"(SongVolume|PreviewVolume)", "real", "DLC Builder's own: the volumes it gave the sound banks"),
    ("builder", "json", r"ArrangementProperties\.Metronome", "real", "DLC Builder's own"),
    ("builder", "json", r"(MaxPhraseDifficulty|Score_MaxNotes|Score_PNV|SongAverageTempo|SongPartition|TargetScore)", "real",
     "DLC Builder gives vocals these (all 0); the DLC's and the Toolkit's vocals have none"),
    ("builder", "json", r"SongPartition", "value", "DLC Builder writes 0; the DLC and the Toolkit 1"),
    ("builder", "json", r"MasterID_(PS3|XBox360)", "value", "DLC Builder repeats MasterID_RDV here; the DLC and the Toolkit have -1"),
    ("builder", "json hsan", r"SKU", "value", 'DLC Builder writes "" in the header and for vocals; the DLC "RS2"'),
    ("builder toolkit", "hsan", r"BassPick", "ours", "the DLC's header has a bass's BassPick; some tools leave it out"),
    ("toolkit", "hsan", r"Tone_(A|B|C|D|Base)", "real", "one Toolkit build copies the tone names into the header; the DLC's header has none"),
    ("toolkit", "json hsan", r"Tuning\.Rs7String6", "real", "a seventh string's tuning: an addition of a 7-string mod's tool, not the game's"),
    ("toolkit builder", "graph", r"sng tag macos", "ours", "the DLC tags a Mac song file macos (psarc.c drops it for the PC); custom songs' tools do not"),
    ("builder", "graph", r"sng logpath", "value", "DLC Builder keeps the platform's folder in a song file's logical path; the DLC does not"),
    ("builder", "graph", r"arrangement xml", "real", "DLC Builder ships the arrangements' XML too; the game reads the .sng"),
    ("builder", "names", r"songs/arr/\w+\.xml", "real", "DLC Builder ships the arrangements' XML too"),
    ("toolkit builder", "names", r"toolkit\.version", "real", "the tools' own mark; not the game's"),
    ("dlc", "names", r"flatmodels/rs/rsenumerable_root\.flat", "ours", "this DLC has no root model; custom songs all do, and we follow the task's list"),
    ("dlc", "appid", r"appid", "value", "the DLC has its own AppID; ours must be the free DLC's (songtext.h)"),
]
used = collections.Counter()


def known(flavour, text, where, how, sample, detail):
    for i, (flavours, texts, pattern, h, _) in enumerate(KNOWN):
        if flavour in flavours.split() and text in texts.split() and h == how and re.fullmatch(pattern, where):
            used[i] += 1
            return
    sys.exit("FAIL: %s: %s: %s: %s %s" % (sample, text, where, how, detail))


def same_number(a, b):
    return abs(a - b) <= 1e-6 * max(1.0, abs(b))


def diff(ours, real, where, out):
    """Every difference between two JSON values as (where, how, detail)"""
    num = lambda v: isinstance(v, (int, float)) and not isinstance(v, bool)
    if isinstance(ours, dict) and isinstance(real, dict):
        for k in ours.keys() - real.keys():
            out.append((where + k, "ours", repr(ours[k])[:80]))
        for k in real.keys() - ours.keys():
            out.append((where + k, "real", repr(real[k])[:80]))
        for k in ours.keys() & real.keys():
            diff(ours[k], real[k], where + k + ".", out)
    elif isinstance(ours, list) and isinstance(real, list):
        if len(ours) != len(real):
            out.append((where.rstrip("."), "value", "%d items, real %d" % (len(ours), len(real))))
        for i, (a, b) in enumerate(zip(ours, real)):
            diff(a, b, where + "%d." % i, out)
    elif num(ours) and num(real):  # an integer or not: the real archives do not agree among themselves (92.0, 92)
        if not same_number(ours, real):
            out.append((where.rstrip("."), "value", "%r, real %r" % (ours, real)))
    elif type(ours) is not type(real) or ours != real:
        out.append((where.rstrip("."), "value", "%r, real %r" % (ours, real)))


def snake(name):
    return re.sub(r"([A-Z])", lambda m: "_" + m.group(1).lower(), name)


def spec_of(real, key, order, wems):
    """A song description from a real archive's manifests: only what sb_song_info and sb_arr_info carry"""
    first = next(a for a in real.values() if "SongName" in a)
    spec = {"key": key, "title": first["SongName"], "artist": first["ArtistName"], "album": first["AlbumName"], "title_sort": first["SongNameSort"],
            "artist_sort": first["ArtistNameSort"], "album_sort": first["AlbumNameSort"], "year": first["SongYear"], "length": first["SongLength"],
            "converted": first["LastConversionDateTime"], "audio_id": wems[0], "preview_id": wems[1], "arr": []}
    for pid in order:
        a = real[pid]
        e = {"name": a["ManifestUrn"].rsplit("_", 1)[1], "pid": pid, "master_id": a["MasterID_RDV"]}
        if "ArrangementType" not in a:
            e["type"] = 4
        else:
            p = a["ArrangementProperties"]
            levels = lambda d: [[int(dk), int(ik), ids] for dk in sorted(d, key=int) for ik, ids in sorted(d[dk].items(), key=lambda kv: int(kv[0]))]
            e.update(type=a["ArrangementType"], path=p["routeMask"], bonus=p["bonusArr"], alternate=int(not p["represent"] and not p["bonusArr"]),
                     tuning=[a["Tuning"]["string%d" % i] for i in range(6)], capo=int(a.get("CapoFret", 0)), cents=a["CentOffset"], tempo=a["SongAverageTempo"],
                     start_time=-a["SongOffset"], notes=[int(a["NotesEasy"]), int(a["NotesMedium"]), int(a["NotesHard"])], max_difficulty=a["MaxPhraseDifficulty"],
                     diff=[a["SongDiffEasy"], a["SongDiffMed"], a["SongDiffHard"]], dna=[a["DNA_Chords"], a["DNA_Riffs"], a["DNA_Solo"]],
                     density=a["DynamicVisualDensity"], props={snake(k): v for k, v in p.items()},
                     phrases=[[x["Name"], x["MaxDifficulty"]] for x in a["Phrases"]], iters=[[x["PhraseIndex"], x["StartTime"]] for x in a["PhraseIterations"]],
                     sections=[[x["Name"], x["StartTime"]] for x in a["Sections"]],
                     chord_templates=[[x["ChordId"], x["ChordName"], x["Fingers"], x["Frets"]] for x in a["ChordTemplates"]],
                     chords=levels(a["Chords"]), techniques=levels(a["Techniques"]), tones=[json.dumps(t) for t in a["Tones"]],
                     tone_base=a["Tone_Base"], tone_a=a["Tone_A"], tone_b=a["Tone_B"], tone_c=a["Tone_C"], tone_d=a["Tone_D"], tone_multiplayer=a["Tone_Multiplayer"])
            ok(a["Tone_Base"] in [t["Key"] for t in a["Tones"]] and all(a[k] in [""] + [t["Key"] for t in a["Tones"]] for k in ("Tone_A", "Tone_B", "Tone_C", "Tone_D")),
               "a real manifest names its tones by their keys")
        spec["arr"].append(e)
    return spec


def sample(binary, px, read_all, path, tmp, n):
    name = os.path.basename(path)
    platform = "mac" if name.endswith("_m.psarc") else "pc"
    files = collections.OrderedDict((p, d) for p, d, _ in read_all(px, path))
    real_xblock = next(d for p, d in files.items() if p.endswith(".xblock"))
    flavour = "toolkit" if "toolkit.version" in files and real_xblock.startswith(b"\xef\xbb\xbf") else "builder" if "toolkit.version" in files else "dlc"
    real_head, _ = attributes(next(d for p, d in files.items() if p.endswith(".hsan")))
    real_json = {}  # persistent ID -> (attributes, the rest of the top level)
    for p, d in files.items():
        if p.startswith("manifests/") and p.endswith(".json"):
            a, top = attributes(d)
            real_json.update({k: (v, top) for k, v in a.items()})
    real_ents = xblock_entities(real_xblock)
    order = [e["id"].upper() for e, _ in real_ents]  # the arrangements in the xblock's order
    key = next(iter(real_head.values()))["SongKey"]
    banks = {p: d for p, d in files.items() if p.endswith(".bnk")}
    song_bank = next(d for p, d in banks.items() if not p.endswith("_preview.bnk"))
    wems = sorted((int(os.path.basename(p)[:-4]) for p in files if p.endswith(".wem")), key=lambda i: struct.pack("<I", i) not in song_bank)
    spec = spec_of({k: v for k, (v, _) in real_json.items()}, key, order, wems)
    m = gen(binary, spec, tmp, "sample%d" % n)
    well_formed(m, name, spec)
    ok(m.key == key and [p for _, p in m.arr] == order, name + ": the key and the persistent IDs given")
    lkey = key.lower()

    # manifests and header: value by value
    count = 0
    for arr_name, pid in m.arr:
        ours, top = attributes(m.text[platform]["manifests/songs_dlc_%s/%s_%s.json" % (lkey, lkey, arr_name)])
        d = []
        diff({"Attributes": ours[pid], **top}, {"Attributes": real_json[pid][0], **real_json[pid][1]}, "", d)
        for where, how, detail in d:
            known(flavour, "json", re.sub(r"^Attributes\.", "", where), how, name, arr_name + ": " + detail)
        count += len(ours[pid])
    ours, top = attributes(m.one(platform, ".hsan"))
    d = []
    diff(ours, real_head, "", d)
    for where, how, detail in d:
        known(flavour, "hsan", where.split(".", 1)[1], how, name, detail)
    count += sum(len(a) for a in ours.values())

    # the xblock: the same entities
    canon = lambda ents: sorted((sorted(e.items()), sorted(p.items())) for e, p in ents)
    ok(canon(xblock_entities(m.one(platform, ".xblock"))) == canon(real_ents), name + ": the xblock's entities are the real ones")
    if flavour != "builder":  # DLC Builder's has no byte order mark and another XML declaration
        ok(m.one(platform, ".xblock") == real_xblock, name + ": the xblock is the real one byte for byte")

    # the graph: the same nodes
    ours, real = graph_nodes(m.one(platform, "_aggregategraph.nt")), graph_nodes(next(d for p, d in files.items() if p.endswith(".nt")))
    for rel in real.keys() - ours.keys():
        ok(re.fullmatch(r"/songs/arr/\w+\.xml", rel), name + ": the real graph has " + rel)
        known(flavour, "graph", "arrangement xml", "real", name, rel)
    ok(not ours.keys() - real.keys(), "%s: our graph has files the real one lacks: %s" % (name, ours.keys() - real.keys()))
    for rel in ours.keys() & real.keys():
        a, b, kind = ours[rel], real[rel], os.path.splitext(rel)[1][1:]
        ok(a["canonical"] == b["canonical"] and a["name"] == b["name"] and (a["llid"] is None) == (b["llid"] is None), "%s: the graph's node of %s" % (name, rel))
        for t in a["tags"] - b["tags"]:
            known(flavour, "graph", "%s tag %s" % (kind, t), "ours", name, rel)
        ok(not b["tags"] - a["tags"], "%s: the real graph tags %s %s" % (name, rel, b["tags"] - a["tags"]))
        if a["logpath"] != b["logpath"]:
            known(flavour, "graph", kind + " logpath", "value", name, "%s, real %s" % (a["logpath"], b["logpath"]))

    # the entries' names, the flat models, the AppID, the events
    ours_names, real_names = {p for _, _, _, p in m.paths[platform]}, set(files)
    for p in real_names - ours_names:
        known(flavour, "names", p, "real", name, "")
    for p in ours_names - real_names:
        known(flavour, "names", p, "ours", name, "")
    for f in ("flatmodels/rs/rsenumerable_root.flat", "flatmodels/rs/rsenumerable_song.flat"):
        if f in files:  # the Mac's custom songs have them with other line ends
            ok(m.text[platform][f].replace(b"\r\n", b"\n") == files[f].replace(b"\r\n", b"\n"), "%s: %s is the real one" % (name, f))
            ok(m.text[platform][f] == files[f] or b"\r\n" not in files[f], "%s: %s is the real one byte for byte" % (name, f))
    if files["appid.appid"] != b"248750":
        known(flavour, "appid", "appid", "value", name, repr(files["appid.appid"]))
    for (event, wwise), (p, bank) in zip(m.events, sorted(banks.items(), key=lambda kv: kv[0].endswith("_preview.bnk"))):
        ok(struct.pack("<I", wwise) in bank, "%s: %s's Wwise ID is in %s" % (name, event, p))
    real_lights = ET.fromstring(next(d for p, d in files.items() if p.endswith("_showlights.xml")).decode("utf-8-sig"))
    ok(real_lights.tag == "showlights" and {l.tag for l in real_lights} == {"showlight"} and set(real_lights[0].attrib) == {"time", "note"}, name + ": the real showlights' form")
    first = next(a for a in spec["arr"] if a.get("sections"))
    ok(float(ET.fromstring(m.one(platform, "_showlights.xml").decode())[0].get("time")) == first["sections"][0][1], name + ": the lights start with the first section")
    print("%s (%s, %s): %d arrangements, %d attributes compared" % (name, flavour, platform, len(m.arr), count))
    return key


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("songtexttest")
    ap.add_argument("--tools")
    ap.add_argument("--samples")
    a = ap.parse_args()
    here = os.path.dirname(os.path.abspath(__file__))
    r = subprocess.run([a.songtexttest, "self"], capture_output=True, text=True)
    ok(r.returncode == 0 and r.stdout.startswith("ok, "), "songtexttest self: %s%s" % (r.stdout, r.stderr[-3000:]))
    print("songtexttest self:", r.stdout.strip())
    with tempfile.TemporaryDirectory() as tmp:
        made_up(a.songtexttest, tmp)
        if a.samples:
            ok(a.tools, "--samples needs --tools")
            sys.path.insert(0, here)
            sys.dont_write_bytecode = True  # no tests/__pycache__ for the reader's import
            from run_varc_test import reader, read_all
            px = reader(a.tools, os.path.join(here, "..", "src", "keys.h"))
            names = sorted(n for n in os.listdir(a.samples) if n.endswith(".psarc"))
            for n, name in enumerate(names):
                sample(a.songtexttest, px, read_all, os.path.join(a.samples, name), tmp, n)
            print("%d sample archives; the differences from them, all known:" % len(names))
            for i, (flavours, texts, pattern, how, why) in enumerate(KNOWN):
                if used[i]:
                    print("  %3d x %-8s %-5s %s: %s" % (used[i], texts, how, pattern, why))
    print("ok, %d checks" % checks)


if __name__ == "__main__":
    main()
