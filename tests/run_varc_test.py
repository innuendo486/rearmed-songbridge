#!/usr/bin/env python3
"""The archive assembler's test: tests/run_varc_test.py VARCTEST --tools DIR [--samples DIR]

Archives are built by varctest from loose files and read back with rs-rearmed's reader (tools/psarc_extract.py in
DIR, given this repository's keys): every entry must come back as the file it was made from, in the order given.
Made-up files cover the sizes that matter (a whole block, a short last block that starts as a zlib stream, one that
does not pack). With --samples, every archive in that folder is also taken apart and built again."""
import argparse, os, random, re, struct, subprocess, sys, tempfile, zlib

BLOCK = 0x10000
checks = 0


def ok(cond, what):
    global checks
    checks += 1
    if not cond:
        sys.exit("FAIL: " + what)


def reader(tools, keys_h):
    sys.path.insert(0, tools)
    import psarc_extract
    t = open(keys_h).read()

    def arr(name):
        m = re.search(name + r"\[\d+\]\s*=\s*\{([^}]*)\}", t)
        return bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", m.group(1))).hex()

    key, iv = arr("sb_toc_key"), arr("sb_toc_iv")
    psarc_extract.decrypt_toc = lambda data, path: subprocess.run(
        ["openssl", "enc", "-d", "-aes-256-cfb", "-K", key, "-iv", iv, "-nopad"], input=data, capture_output=True, check=True).stdout
    return psarc_extract


def read_all(px, path):
    """[(name, data)] in the archive's order, and the block sizes of each entry"""
    f, bs, entries, zs = px.read_archive(path)
    names = px.read_entry(f, bs, zs, entries[0]).decode().split("\n")
    ok(len(names) == len(entries) - 1, "a name for every entry")
    out = []
    for i, n in enumerate(names):
        zi, length, off = entries[i + 1]
        nb = (length + bs - 1) // bs
        out.append((n, px.read_entry(f, bs, zs, entries[i + 1]), zs[zi:zi + nb]))
    f.close()
    return out


def build(varctest, px, files, tmp, tag):
    """files: [(name, data)] -> the archive built from them, read back"""
    d = os.path.join(tmp, tag)
    for n, data in files:
        p = os.path.join(d, n)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        open(p, "wb").write(data)
    open(os.path.join(d, "names.txt"), "w").write("\n".join(n for n, _ in files) + "\n")
    out = os.path.join(tmp, tag + ".psarc")
    r = subprocess.run([varctest, d, out], capture_output=True, text=True)
    ok(r.returncode == 0, "%s: varctest: %s%s" % (tag, r.stdout, r.stderr))
    back = read_all(px, out)
    ok([n for n, _, _ in back] == [n for n, _ in files], tag + ": the names in the order given")
    for (n, data), (_, got, zl) in zip(files, back):
        ok(got == data, "%s: %s comes back as it went in" % (tag, n))
        last = len(data) % BLOCK
        ok(all(z == 0 for z in zl[:-1 if last else None]), "%s: %s: whole blocks are stored" % (tag, n))
        if last:
            tail = data[len(data) - last:]
            zlibish = len(tail) >= 2 and tail[0] & 15 == 8 and tail[0] >> 4 <= 7 and (tail[0] << 8 | tail[1]) % 31 == 0
            ok((zl[-1] != last) == zlibish, "%s: %s: a short last block is packed only when it starts as a zlib stream" % (tag, n))
    return r.stdout.strip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("varctest")
    ap.add_argument("--tools", required=True)
    ap.add_argument("--samples")
    a = ap.parse_args()
    here = os.path.dirname(os.path.abspath(__file__))
    px = reader(a.tools, os.path.join(here, "..", "src", "keys.h"))
    rng = random.Random(7)
    noise = lambda n: bytes(rng.getrandbits(8) for _ in range(n))
    made = [
        ("one/byte.bin", b"x"),
        ("one/block.bin", noise(BLOCK)),
        ("two/blocks.bin", noise(2 * BLOCK)),
        ("text/manifest.json", b'{"a": 1}' * 9000),
        ("tail/zlibish-packs.bin", noise(BLOCK) + b"\x78\x9c" + b"A" * 3000),
        ("tail/zlibish-noise.bin", noise(BLOCK) + b"\x78\x9c" + noise(500)),
        ("tail/real-zlib.bin", zlib.compress(b"song " * 5000)),
        ("tail/short-zlibish.bin", b"\x78\x9c"),
        ("big/audio.wem", noise(5 * BLOCK + 12345)),
        ("appid.appid", b"248750"),
    ]
    with tempfile.TemporaryDirectory() as tmp:
        print("made-up files:", build(a.varctest, px, made, tmp, "made"))
        print("one entry:", build(a.varctest, px, [("appid.appid", b"248750")], tmp, "single"))
        if a.samples:
            n = 0
            for name in sorted(os.listdir(a.samples)):
                if not name.endswith(".psarc"):
                    continue
                src = read_all(px, os.path.join(a.samples, name))
                build(a.varctest, px, [(n_, d) for n_, d, _ in src], tmp, "s%d" % n)
                n += 1
            print("%d sample archives taken apart and built again" % n)
    print("ok, %d checks" % checks)


if __name__ == "__main__":
    main()
