# Disassemble CrimsonDesert.exe by RVA. Usage: python disasm.py <rva-hex> [count] [--func]  (--func: start at containing function via .pdata)
import sys, struct, bisect, capstone
EXE = r"D:\SteamLibrary\steamapps\common\Crimson Desert\bin64\CrimsonDesert.exe"
IB = 0x140000000
data = open(EXE, "rb").read()
e = struct.unpack_from("<I", data, 0x3c)[0]
nsec = struct.unpack_from("<H", data, e+6)[0]; optsz = struct.unpack_from("<H", data, e+20)[0]
opt = e+24
exc_rva, exc_size = struct.unpack_from("<II", data, opt+0x70+3*8)
secs = []
for i in range(nsec):
    s = data[opt+optsz+i*40:][:40]
    vs, va, rs, ro = struct.unpack_from("<IIII", s, 8)
    secs.append((s[:8].rstrip(b"\0").decode(errors="replace"), va, vs, ro, rs))
def rva2off(r):
    for name, va, vs, ro, rs in secs:
        if va <= r < va+rs: return ro + (r-va)
_funcs = None
def funcs():
    global _funcs
    if _funcs is None:
        off = rva2off(exc_rva); n = exc_size//12
        recs = [struct.unpack_from("<III", data, off+i*12) for i in range(n)]
        recs = sorted(r for r in recs if r[0])
        _funcs = ([r[0] for r in recs], recs)
    return _funcs
def func_of(rva):
    begins, recs = funcs()
    i = bisect.bisect_right(begins, rva)-1
    if i >= 0 and recs[i][0] <= rva < recs[i][1]: return recs[i][0], recs[i][1]
    return None
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
def disasm(rva, count=100, until_ret=False):
    off = rva2off(rva); out = []
    for ins in md.disasm(data[off:off+count*16], IB+rva):
        out.append((ins.address-IB, ins.mnemonic, ins.op_str, ins.bytes.hex()))
        if len(out) >= count or (until_ret and ins.mnemonic == "ret"): break
    return out
if __name__ == "__main__":
    rva = int(sys.argv[1], 16); cnt = int(sys.argv[2]) if len(sys.argv) > 2 and not sys.argv[2].startswith("--") else 80
    if "--func" in sys.argv:
        f = func_of(rva); print("function bounds:", (hex(f[0]), hex(f[1])) if f else None)
        if f: rva = f[0]; cnt = min(cnt, (f[1]-f[0])//1+1)
    for a, m, o, b in disasm(rva, cnt):
        print("  +0x%08x  %-8s %s" % (a, m, o))
