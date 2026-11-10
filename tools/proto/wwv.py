"""Wwise Vorbis (.wem) as Rocksmith 2014 has it: reading (for study) and the pieces shared with ogg2wem.py."""
import struct, sys

class R:
    def __init__(s, d, p=0): s.d, s.p = d, p
    def get(s, n):
        v = 0
        for i in range(n):
            v |= ((s.d[s.p >> 3] >> (s.p & 7)) & 1) << i; s.p += 1
        return v
class W:
    def __init__(s): s.b = bytearray(); s.n = 0
    def put(s, v, n):
        for i in range(n):
            if s.n & 7 == 0: s.b.append(0)
            if (v >> i) & 1: s.b[-1] |= 1 << (s.n & 7)
            s.n += 1
def ilog(v):
    n = 0
    while v: n += 1; v >>= 1
    return n

def copy_floor(r, w):
    parts = r.get(5); w.put(parts, 5); cls = []
    for _ in range(parts): c = r.get(4); w.put(c, 4); cls.append(c)
    dims = []
    for _ in range(max(cls) + 1 if cls else 0):
        d = r.get(3); w.put(d, 3); dims.append(d + 1)
        sub = r.get(2); w.put(sub, 2)
        if sub: w.put(r.get(8), 8)
        for _ in range(1 << sub): w.put(r.get(8), 8)
    w.put(r.get(2), 2); rb = r.get(4); w.put(rb, 4)
    for c in cls:
        for _ in range(dims[c]): w.put(r.get(rb), rb)
def copy_residue(r, w):
    w.put(r.get(24), 24); w.put(r.get(24), 24); w.put(r.get(24), 24)
    ncl = r.get(6); w.put(ncl, 6); w.put(r.get(8), 8); casc = []
    for _ in range(ncl + 1):
        lo = r.get(3); w.put(lo, 3); f = r.get(1); w.put(f, 1); hi = 0
        if f: hi = r.get(5); w.put(hi, 5)
        casc.append(hi * 8 + lo)
    for c in casc:
        for k in range(8):
            if c & (1 << k): w.put(r.get(8), 8)
def copy_mapping(r, w, ch):
    f = r.get(1); w.put(f, 1); sub = 1
    if f: v = r.get(4); w.put(v, 4); sub = v + 1
    f = r.get(1); w.put(f, 1)
    if f:
        steps = r.get(8); w.put(steps, 8)
        for _ in range(steps + 1):
            n = ilog(ch - 1); w.put(r.get(n), n); w.put(r.get(n), n)
    w.put(r.get(2), 2)
    if sub > 1:
        for _ in range(ch): w.put(r.get(4), 4)
    for _ in range(sub): w.put(r.get(8), 8); w.put(r.get(8), 8); w.put(r.get(8), 8)

def parse_ww_setup(pkt, ch):
    """A Wwise setup packet -> (codebook ids, mode blockflags)."""
    r = R(pkt); w = W(); ids = [r.get(10) for _ in range(r.get(8) + 1)]
    nf = r.get(6) + 1
    for _ in range(nf): copy_floor(r, w)
    nr = r.get(6) + 1; rtypes = []
    for _ in range(nr): rtypes.append(r.get(2)); copy_residue(r, w)
    nm = r.get(6) + 1
    for _ in range(nm): copy_mapping(r, w, ch)
    modes = []
    for _ in range(r.get(6) + 1): modes.append(r.get(1)); r.get(8)
    return dict(ids=ids, floors=nf, residues=nr, rtypes=rtypes, mappings=nm, modes=modes, bits=r.p, size=len(pkt))

def read_wem(path):
    d = open(path, 'rb').read(); o = 12; ck = {}
    while o + 8 <= len(d):
        n = struct.unpack_from('<I', d, o + 4)[0]; ck[d[o:o+4]] = (o + 8, n); o += 8 + n + (n & 1)
    fo, fn = ck[b'fmt ']; f = d[fo:fo+fn]
    tag, ch, rate, avg, align, bits, cb = struct.unpack_from('<HHIIHHH', f, 0)
    h = dict(ch=ch, rate=rate, avg=avg, mask=struct.unpack_from('<I', f, 0x14)[0])
    (h['frames'], h['loop_start'], h['loop_end'], h['loop_begin_extra'], h['loop_end_extra'], h['seek_size'], h['data_off'],
     h['max_packet'], h['last_extra'], h['alloc'], h['alloc64'], h['hash'], bs0, bs1) = struct.unpack_from('<IIIHHIIHHIIIBB', f, 0x18)
    h['bs'] = (bs0, bs1); do, dn = ck[b'data']; data = d[do:do+dn]
    h['data_size'] = dn; h['chunks'] = [k.decode() for k in ck]
    seek = [struct.unpack_from('<HH', data, i) for i in range(0, h['seek_size'], 4)]
    so = h['seek_size']; sn = struct.unpack_from('<H', data, so)[0]; setup = data[so+2:so+2+sn]
    pk = []; o = h['data_off']
    while o + 2 <= dn:
        n = struct.unpack_from('<H', data, o)[0]; pk.append((o, data[o+2:o+2+n])); o += 2 + n
    return h, seek, setup, pk, data

if __name__ == '__main__':
    for p in sys.argv[1:]:
        h, seek, setup, pk, data = read_wem(p); s = parse_ww_setup(setup, h['ch'])
        print(p.split('/')[-1], {k: (hex(v) if k == 'hash' else v) for k, v in h.items()})
        print('  setup: %d bytes, parsed %d bits, %d codebooks, floors %d residues %d %s mappings %d modes %s' % (s['size'], s['bits'], len(s['ids']), s['floors'], s['residues'], s['rtypes'], s['mappings'], s['modes']))
        mb = ilog(len(s['modes']) - 1); bs = [1 << h['bs'][0], 1 << h['bs'][1]]; tot = 0; prev = None; per = []
        for o, p_ in pk:
            cur = bs[s['modes'][R(p_).get(mb)]] if p_ else None
            if cur is None: per.append(0); continue
            n = 0 if prev is None else (prev + cur) // 4
            tot += n; per.append(n); prev = cur
        print('  packets %d, largest %d, frames by block sizes %d vs header %d (diff %d); last packet ends at %d of %d' % (len(pk), max(len(x) for _, x in pk), tot, h['frames'], tot - h['frames'], pk[-1][0] + 2 + len(pk[-1][1]), h['data_size']))
        print('  first packets (offset, size, frames):', [(o, len(x), n) for (o, x), n in zip(pk[:6], per[:6])])
        print('  seek table: %d entries, first %s, sum frames %d, sum offsets %d' % (len(seek), seek[:5], sum(a for a, _ in seek), sum(b for _, b in seek)))
        # where do the seek entries point?
        offs = {o: i for i, (o, _) in enumerate(pk)}; acc_f = acc_o = 0; cum = [0]
        for n in per: cum.append(cum[-1] + n)
        rows = []
        for a, b in seek[:6]:
            acc_f += a; acc_o += b; i = offs.get(acc_o + h['seek_size']); rows.append((acc_f, acc_o, i, cum[i] if i is not None else None))
        print('  seek entries (frames, offset, packet index, frames decoded before that packet):', rows)
