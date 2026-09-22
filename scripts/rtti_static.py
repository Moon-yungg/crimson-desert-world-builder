# Offline RTTI analysis of CrimsonDesert.exe: TypeDescriptors -> CompleteObjectLocators -> vtables (all as RVAs)
import struct, sys, re, collections, time
EXE = r"D:\SteamLibrary\steamapps\common\Crimson Desert\bin64\CrimsonDesert.exe"
d = open(EXE, "rb").read()
e = struct.unpack_from("<I", d, 0x3c)[0]
nsec = struct.unpack_from("<H", d, e+6)[0]; optsz = struct.unpack_from("<H", d, e+20)[0]
imgbase = struct.unpack_from("<Q", d, e+24+24)[0]
secs = []
off = e+24+optsz
for i in range(nsec):
    s = d[off+i*40:off+i*40+40]
    name = s[:8].rstrip(b"\0").decode(errors="replace")
    vs, va, rs, ro = struct.unpack_from("<IIII", s, 8); ch = struct.unpack_from("<I", s, 36)[0]
    secs.append((name, va, vs, ro, rs, ch))
def rva2off(r):
    for name, va, vs, ro, rs, ch in secs:
        if va <= r < va+rs: return r-va+ro
    return None
def off2rva(o):
    for name, va, vs, ro, rs, ch in secs:
        if ro <= o < ro+rs: return o-ro+va
    return None
def is_code_rva(r):
    for name, va, vs, ro, rs, ch in secs:
        if va <= r < va+vs: return bool(ch & 0x20000000)
    return False
t0 = time.time()
td = {}  # rva(TD) -> name
for m in re.finditer(rb"\.\?A[VU][A-Za-z0-9_@?$<>,\-\. ]{1,900}?\x00", d):
    o = m.start()-16; r = off2rva(o)
    if r is not None: td[r] = m.group()[:-1].decode(errors="replace")
print("type descriptors:", len(td), "(%.1fs)" % (time.time()-t0))
print("imagebase 0x%x" % imgbase)
# COL candidates: dword == some TD rva, check neighbours
t0 = time.time()
cols = {}   # rva(COL) -> (tdname, offset)
stats = collections.Counter()
tdset = set(td)
# fast path: scan all dwords via memoryview / numpy-free approach
import array
for name, va, vs, ro, rs, ch in secs:
    if not (ch & 0x40000000) or (ch & 0x02000000): continue  # readable, not discardable
    buf = d[ro:ro+rs]
    n = len(buf)//4
    arr = array.array("I"); arr.frombytes(buf[:n*4])
    for i, v in enumerate(arr):
        if v in tdset:
            o = ro + i*4 - 12      # pTypeDescriptor is at COL+12
            if o < ro: continue
            sig, coff, cdoff, ptd, pcd, pself = struct.unpack_from("<6I", d, o)
            rva = off2rva(o)
            stats["ptd_hit"] += 1
            if sig == 1: stats["sig1"] += 1
            if pself == rva: stats["pself_ok"] += 1
            if sig == 1 and pself == rva:
                cols[rva] = (td[ptd], coff)
            elif sig == 1 and stats["sig1_pself_bad_examples"] < 5:
                stats["sig1_pself_bad_examples"] += 1
                print("  example sig1 but pself mismatch: COL rva 0x%x pself 0x%x td %s" % (rva, pself, td[ptd][:60]))
print("stats:", dict(stats), "(%.1fs)" % (time.time()-t0))
print("COLs:", len(cols))
# vtables: qword == imgbase + COL rva
t0 = time.time()
colva = {imgbase + r: r for r in cols}
vt = []
for name, va, vs, ro, rs, ch in secs:
    if not (ch & 0x40000000) or (ch & 0x02000000): continue
    buf = d[ro:ro+rs]; n = len(buf)//8
    arr = array.array("Q"); arr.frombytes(buf[:n*8])
    for i, v in enumerate(arr):
        r = colva.get(v)
        if r is None: continue
        vtoff = ro + i*8 + 8
        cnt = 0
        while vtoff + cnt*8 + 8 <= ro+rs:
            f = struct.unpack_from("<Q", d, vtoff+cnt*8)[0]
            if not (imgbase <= f < imgbase + 0x20000000) or not is_code_rva(f-imgbase): break
            cnt += 1
        vt.append((cols[r][0], cols[r][1], off2rva(vtoff), cnt))
print("vtables:", len(vt), "(%.1fs)" % (time.time()-t0))
with open("notes/rtti_static.txt", "w", encoding="utf-8") as f:
    f.write("# imagebase=0x%x format: name | this_offset | vtable_rva | nvfuncs\n" % imgbase)
    for n, o, r, c in sorted(vt): f.write("%s | %d | 0x%x | %d\n" % (n, o, r, c))
for n, o, r, c in vt[:5]: print(n, o, hex(r), c)
