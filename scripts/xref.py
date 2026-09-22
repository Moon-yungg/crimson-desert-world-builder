# Find rip-relative code references (lea/mov/call/jmp with disp32) to a target RVA in CrimsonDesert.exe.
# Usage: python xref.py <rva-hex | string> [--str]   (with --str: find the string, then its xrefs)
import sys, struct, numpy as np, time
EXE = r"D:\SteamLibrary\steamapps\common\Crimson Desert\bin64\CrimsonDesert.exe"
IB = 0x140000000
data = open(EXE, "rb").read()
e = struct.unpack_from("<I", data, 0x3c)[0]
nsec = struct.unpack_from("<H", data, e+6)[0]; optsz = struct.unpack_from("<H", data, e+20)[0]
secs = []
for i in range(nsec):
    s = data[e+24+optsz+i*40:][:40]
    name = s[:8].rstrip(b"\0").decode(errors="replace")
    vs, va, rs, ro = struct.unpack_from("<IIII", s, 8); ch = struct.unpack_from("<I", s, 36)[0]
    secs.append((name, va, vs, ro, rs, ch))
def off2rva(o):
    for name, va, vs, ro, rs, ch in secs:
        if ro <= o < ro+rs: return va + (o-ro)
def rva2off(r):
    for name, va, vs, ro, rs, ch in secs:
        if va <= r < va+rs: return ro + (r-va)

def xrefs(target_rva):
    hits = []
    for name, va, vs, ro, rs, ch in secs:
        if not (ch & 0x20000000): continue   # executable only
        buf = np.frombuffer(data, dtype=np.uint8, count=rs, offset=ro)
        n = rs - 4
        for al in range(4):
            v = buf[al:al + ((n-al)//4)*4].view("<i4")          # int32 at offsets al, al+4, ...
            idx = np.arange(al, al + len(v)*4, 4, dtype=np.int64)
            want = (target_rva - (va + idx + 4)).astype(np.int64)
            m = np.nonzero(v.astype(np.int64) == want)[0]
            for k in m:
                o = ro + int(idx[k])
                hits.append((va + int(idx[k]), name, data[o-3:o]))
    return sorted(hits)

if __name__ == "__main__":
    arg = sys.argv[1]
    if "--str" in sys.argv:
        i = data.find(arg.encode()); assert i >= 0, "string not found"
        target = off2rva(i); print("string at file 0x%x rva 0x%x" % (i, target))
    else:
        target = int(arg, 16)
    t0 = time.time(); h = xrefs(target)
    print("%d rip-relative refs to rva 0x%x (%.1fs)" % (len(h), target, time.time()-t0))
    for rva, name, pre in h[:80]:
        print("  disp at rva 0x%08x (%s) preceding bytes %s" % (rva, name, pre.hex()))
