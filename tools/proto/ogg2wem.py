"""ogg2wem.py <codebooks.bin> <in.ogg> <out.wem> [--like template.wem]
An Ogg Vorbis file as a Wwise Vorbis .wem the way Rocksmith 2014's are, without decoding: the setup header names its
codebooks by their number in the game's library, the packets lose their type bit and window flags."""
import struct, sys, zlib
from wwv import R, W, ilog, copy_floor, copy_residue, copy_mapping, read_wem

def quantvals(entries, dim):
    v = int(entries ** (1.0 / dim))
    while True:
        acc = acc1 = 1
        for _ in range(dim): acc *= v; acc1 *= v + 1
        if acc <= entries < acc1: return v
        v += -1 if acc > entries else 1

def ogg_packets(d):
    """[(packet, granule of the page it ends on or None)]"""
    o = 0; cur = b''; out = []
    while o + 27 <= len(d):
        assert d[o:o+4] == b'OggS', 'not Ogg at %d' % o
        gran = struct.unpack_from('<q', d, o + 6)[0]; nseg = d[o+26]; lac = d[o+27:o+27+nseg]; q = o + 27 + nseg; ended = []
        for l in lac:
            cur += d[q:q+l]; q += l
            if l < 255: ended.append(cur); cur = b''
        for i, p in enumerate(ended): out.append((p, gran if i == len(ended) - 1 else None))
        o = q
    return out

class Library:
    def __init__(s, path):
        lib = open(path, 'rb').read(); assert lib[:4] == b'RSCB'
        s.n = struct.unpack_from('<I', lib, 4)[0]; s.offs = struct.unpack_from('<%dI' % s.n, lib, 8); s.data = lib[8 + 4 * s.n:]; s.idx = {}
        for i in range(s.n): s.idx.setdefault(s.bits(i, 18), []).append(i)
    def bits(s, i, n):
        r = R(s.data, s.offs[i] * 8); return tuple(r.get(1) for _ in range(min(n, (len(s.data) - s.offs[i]) * 8)))
    def find(s, cands):
        for b in cands:
            for i in s.idx.get(tuple(b[:18]), []):
                if s.bits(i, len(b)) == tuple(b): return i
        return None

def book_candidates(r):
    """The standard codebook at r in the library's packed form (one per width of the length field). r is advanced."""
    def bits(v, n): return [(v >> i) & 1 for i in range(n)]
    assert r.get(24) == 0x564342
    dim, entries = r.get(16), r.get(24); ok = dim < 16 and entries < 16384
    head = bits(dim, 4) + bits(entries, 14); ordered = r.get(1); head += [ordered]
    if ordered:
        head += bits(r.get(5), 5); cur = 0
        while cur < entries:
            n = ilog(entries - cur); v = r.get(n); head += bits(v, n); cur += v
        heads = [head]
    else:
        sparse = r.get(1); lens = []
        for _ in range(entries): lens.append(r.get(5) if not sparse or r.get(1) else None)
        mx = max([l for l in lens if l is not None] or [0]); heads = []
        for L in range(max(ilog(mx), 1), 8):
            h = head + bits(L, 3) + [sparse]
            for l in lens:
                if sparse: h.append(0 if l is None else 1)
                if l is not None: h += bits(l, L)
            heads.append(h)
    lt = r.get(4); tail = [lt & 1]; ok = ok and lt <= 1
    if lt == 1:
        tail += bits(r.get(32), 32) + bits(r.get(32), 32); vl = r.get(4); tail += bits(vl, 4) + [r.get(1)]
        for _ in range(quantvals(entries, dim)): tail += bits(r.get(vl + 1), vl + 1)
    elif lt == 2:
        r.get(64); vl = r.get(4); r.get(1)
        for _ in range(entries * dim): r.get(vl + 1)
    return [h + tail for h in heads] if ok else []

def convert_setup(setup, ch, lib):
    r = R(setup); assert r.get(8) == 5 and bytes(r.get(8) for _ in range(6)) == b'vorbis'
    w = W(); n = r.get(8); w.put(n, 8)
    for b in range(n + 1):
        i = lib.find(book_candidates(r))
        if i is None: raise ValueError('codebook %d is not in the game\'s library' % b)
        w.put(i, 10)
    for _ in range(r.get(6) + 1): assert r.get(16) == 0
    n = r.get(6); w.put(n, 6)
    for _ in range(n + 1):
        if r.get(16) != 1: raise ValueError('floor type 0')
        copy_floor(r, w)
    n = r.get(6); w.put(n, 6)
    for _ in range(n + 1):
        t = r.get(16); assert t < 3; w.put(t, 2); copy_residue(r, w)
    n = r.get(6); w.put(n, 6)
    for _ in range(n + 1):
        assert r.get(16) == 0; copy_mapping(r, w, ch)
    n = r.get(6); w.put(n, 6); modes = []
    for _ in range(n + 1):
        f = r.get(1); assert r.get(16) == 0 and r.get(16) == 0; m = r.get(8); w.put(f, 1); w.put(m, 8); modes.append(f)
    assert r.get(1) == 1, 'framing'
    return bytes(w.b), modes

def convert_packet(p, modes, mb):
    r = R(p)
    if r.get(1): return None          # not audio
    w = W(); m = r.get(mb); w.put(m, mb)
    if modes[m]: r.get(2)
    left = len(p) * 8 - r.p
    while left > 0:
        n = min(left, 32); w.put(r.get(n), n); left -= n
    return bytes(w.b), modes[m]

def convert(ogg, lib, like=None):
    pk = ogg_packets(ogg); ident = pk[0][0]; assert ident[:7] == b'\x01vorbis'
    ch, rate = ident[11], struct.unpack_from('<I', ident, 12)[0]; bs0, bs1 = ident[28] & 15, ident[28] >> 4
    setup, modes = convert_setup(pk[2][0], ch, lib); mb = ilog(len(modes) - 1); bs = (1 << bs0, 1 << bs1)
    out = []; prev = None; total = 0; last_gran = 0; cum = [0]
    for p, gran in pk[3:]:
        c = convert_packet(p, modes, mb)
        if c is None: continue
        body, flag = c; cur = bs[flag]; total += 0 if prev is None else (prev + cur) // 4; prev = cur
        out.append(body); cum.append(total)
        if gran is not None and gran >= 0: last_gran = gran
    frames = last_gran if 0 < last_gran <= total else total; extra = total - frames
    assert extra < 65536 and max(len(b) for b in out) < 65536
    # the seek table: about every 16384 frames, the frames decoded through a packet and where the packet is
    body = bytearray(struct.pack('<H', len(setup)) + setup); first = len(body); offs = []
    for b in out: offs.append(len(body)); body += struct.pack('<H', len(b)) + b
    seek = bytearray(); sf = so = 0
    for i in range(1, len(out)):
        f = cum[i + 1]
        if f - sf >= 16384 or offs[i] - so >= 0xc000:
            seek += struct.pack('<HH', f - sf, offs[i] - so); sf, so = f, offs[i]
    data = bytes(seek) + bytes(body)
    alloc, alloc64, hsh = 0x10000, 0x10000, zlib.crc32(setup)   # the decoder's set-up block (an upper bound) and the key it is shared by
    if like:
        alloc, alloc64, hsh = like['alloc'], like['alloc64'], like['hash']
    mask = {1: 4, 2: 3}.get(ch, (1 << ch) - 1)
    fmt = struct.pack('<HHIIHHHHI', 0xFFFF, ch, rate, len(data) * rate // max(frames, 1), 0, 0, 0x30, 0, mask)
    fmt += struct.pack('<IIIHHIIHHIIIBB', frames, first, len(body), 0, extra, len(seek), len(seek) + first,
                       max(len(b) for b in out), extra, alloc, alloc64, hsh, bs0, bs1)
    assert len(fmt) == 0x42
    riff = b'WAVE' + b'fmt ' + struct.pack('<I', len(fmt)) + fmt + b'data' + struct.pack('<I', len(data)) + data
    return b'RIFF' + struct.pack('<I', len(riff)) + riff, dict(ch=ch, rate=rate, frames=frames, extra=extra, packets=len(out), setup=setup, modes=modes, bs=(bs0, bs1))

if __name__ == '__main__':
    lib = Library(sys.argv[1]); like = None
    if '--like' in sys.argv: like = read_wem(sys.argv[sys.argv.index('--like') + 1])[0]
    wem, info = convert(open(sys.argv[2], 'rb').read(), lib, like)
    open(sys.argv[3], 'wb').write(wem)
    print({k: (v.hex()[:40] + '...' if k == 'setup' else v) for k, v in info.items()}, len(wem), 'bytes')
