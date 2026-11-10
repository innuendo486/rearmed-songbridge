"""trace.py <tools> <keys.h> <archive.psarc> <trace.log>: the game's reads of the archive, by entry and phase."""
import re, subprocess, struct, sys
tools, keys_h, arc, log = sys.argv[1:5]
sys.path.insert(0, tools)
import psarc_extract
t = open(keys_h).read()
def arr(name):
    m = re.search(name + r"\[\d+\]\s*=\s*\{([^}]*)\}", t); return bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", m.group(1))).hex()
key, iv = arr("sb_toc_key"), arr("sb_toc_iv")
psarc_extract.decrypt_toc = lambda data, path: subprocess.run(["openssl", "enc", "-d", "-aes-256-cfb", "-K", key, "-iv", iv, "-nopad"], input=data, capture_output=True, check=True).stdout
f, bs, entries, zs = psarc_extract.read_archive(arc)
names = ["(name list)"] + psarc_extract.read_entry(f, bs, zs, entries[0]).decode().split("\n")
f.seek(0); toc_len = struct.unpack(">4sI4sIIIII", f.read(32))[3]
spans = []   # (start, end, name, length)
for i, (zi, length, off) in enumerate(entries):
    n = 0; got = 0; z = zi
    while got < length: n += zs[z] or bs; got += bs; z += 1
    spans.append((off, off + n, names[i], length))
spans.sort()
def where(off):
    if off < 32: return "header"
    if off < toc_len: return "table of contents"
    for s, e, n, _ in spans:
        if s <= off < e: return n
    return "past the end"
base = arc.split("/")[-1].lower(); ids = set(); ev = []
for line in open(log):
    p = line.split()
    if p[1] == "open" and base in line.lower(): ids.add(p[2]); ev.append((float(p[0]), "open", 0, 0))
    elif p[1] == "read" and p[2] in ids: ev.append((float(p[0]), "read", int(p[3]), int(p[4])))
    elif p[1] == "close" and p[2] in ids: ev.append((float(p[0]), "close", 0, 0))
print(arc.split("/")[-1], "toc ends at", toc_len, "size", spans[-1][1], "events", len(ev))
# phases: a gap of 3 s or more starts a new one
phase = []; last = None
for e in ev:
    if last is None or e[0] - last > 3000: phase.append([])
    phase[-1].append(e); last = e[0]
size = {n: (e - s, l) for s, e, n, l in spans}
base_of = {n: s for s, e, n, l in spans}
for ph in phase:
    reads = [e for e in ph if e[1] == "read"]
    print("\nphase %.1f s - %.1f s: %d reads, %d bytes, %d opens" % (ph[0][0] / 1000, ph[-1][0] / 1000, len(reads), sum(e[3] for e in reads), sum(1 for e in ph if e[1] == "open")))
    agg = {}; order = []
    for _, _, off, n in reads:
        w = where(off)
        if w not in agg: agg[w] = [0, 0, off, off + n]; order.append(w)
        a = agg[w]; a[0] += 1; a[1] += n; a[2] = min(a[2], off); a[3] = max(a[3], off + n)
    for w in order:
        a = agg[w]; stored = size.get(w, (0, 0))[0]
        print("   %-62s %4d reads %9d bytes%s" % (w[-62:], a[0], a[1], "  (of %d stored)" % stored if stored else ""))

print("\nin order:")
last = None; run = None
def flush():
    if run: print("  %7.1f s  %-58s x%-3d %9d bytes  offsets %d..%d of stored %d" % (run[0] / 1000, run[1][-58:], run[2], run[3], run[4], run[5], size.get(run[1], (0, 0))[0]))
for tm, kind, off, n in ev:
    if kind != "read": continue
    w = where(off)
    if run and run[1] == w and tm - run[6] < 3000:
        run[2] += 1; run[3] += n; run[4] = min(run[4], off - base_of.get(w, 0)); run[5] = max(run[5], off + n - base_of.get(w, 0)); run[6] = tm
    else:
        flush(); run = [tm, w, 1, n, off - base_of.get(w, 0), off + n - base_of.get(w, 0), tm]
flush()
