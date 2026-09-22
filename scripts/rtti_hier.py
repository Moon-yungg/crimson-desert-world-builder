# RTTI class hierarchy from CrimsonDesert.exe: for every COL, list base classes (via ClassHierarchyDescriptor -> BaseClassArray)
import struct, re, sys, collections
EXE = r"D:\SteamLibrary\steamapps\common\Crimson Desert\bin64\CrimsonDesert.exe"
d = open(EXE, "rb").read()
e = struct.unpack_from("<I", d, 0x3c)[0]; nsec = struct.unpack_from("<H", d, e+6)[0]; optsz = struct.unpack_from("<H", d, e+20)[0]
secs = []
for i in range(nsec):
    s = d[e+24+optsz+i*40:][:40]; vs, va, rs, ro = struct.unpack_from("<IIII", s, 8); secs.append((va, vs, ro, rs))
def r2o(r):
    for va, vs, ro, rs in secs:
        if va <= r < va+rs: return ro + r - va
def name_of_td(td_rva):
    o = r2o(td_rva)
    if o is None: return None
    end = d.find(b"\0", o+16, o+1200)
    n = d[o+16:end]
    return n.decode(errors="replace") if n.startswith(b".?A") else None
# collect COLs from rtti_static output (name | off | vtable_rva | n) is not enough; rescan COLs quickly by pSelf check around TD hits
cols = {}
for m in re.finditer(rb"\.\?A[VU]", d):
    td_off = m.start()-16
    pass
# faster: reuse notes/rtti_static.txt vtables -> COL = qword before vtable
out = {}
for line in open("notes/rtti_static.txt", encoding="utf-8"):
    if line.startswith("#"): continue
    name, off, vt, n = [x.strip() for x in line.split("|")]
    if off != "0": continue
    vt_rva = int(vt, 16); o = r2o(vt_rva-8)
    col_va = struct.unpack_from("<Q", d, o)[0]; col_rva = col_va - 0x140000000
    co = r2o(col_rva)
    sig, coff, cdoff, ptd, pchd, pself = struct.unpack_from("<6I", d, co)
    ho = r2o(pchd)
    csig, attrs, nbase, pbca = struct.unpack_from("<4I", d, ho)
    bo = r2o(pbca)
    bases = []
    for i in range(nbase):
        pbcd = struct.unpack_from("<I", d, bo+4*i)[0]
        bdo = r2o(pbcd)
        btd = struct.unpack_from("<I", d, bdo)[0]
        bases.append(name_of_td(btd))
    out[name] = bases
with open("notes/rtti_hierarchy.txt", "w", encoding="utf-8") as f:
    for name, bases in sorted(out.items()):
        f.write(name + " : " + " , ".join(b for b in bases[1:] if b) + "\n")
print("classes:", len(out))
q = sys.argv[1] if len(sys.argv) > 1 else None
if q:
    print("=== derived from", q, "===")
    for name, bases in sorted(out.items()):
        if any(b and q in b for b in bases[1:]): print(" ", name, "<-", [b for b in bases[1:] if b][:4])
    print("=== bases of classes matching", q, "===")
    for name, bases in sorted(out.items()):
        if q in name: print(" ", name, ":", [b for b in bases[1:] if b])
