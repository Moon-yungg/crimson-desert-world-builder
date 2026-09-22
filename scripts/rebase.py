# Re-locate known functions in a new game build: take the old function's first instructions, wildcard rel32/rip-relative
# displacements and immediates that look like addresses, and search the new exe. Usage: python rebase.py
import sys, re, struct, capstone
OLD = r"D:\SteamLibrary\steamapps\common\Crimson Desert\bin64\CrimsonDesert_old.exe"
NEW = r"D:\SteamLibrary\steamapps\common\Crimson Desert\bin64\CrimsonDesert.exe"
def load(path):
    d = open(path, "rb").read()
    e = struct.unpack_from("<I", d, 0x3c)[0]; nsec = struct.unpack_from("<H", d, e+6)[0]; optsz = struct.unpack_from("<H", d, e+20)[0]
    secs = []
    for i in range(nsec):
        s = d[e+24+optsz+i*40:][:40]; vs, va, rs, ro = struct.unpack_from("<IIII", s, 8); ch = struct.unpack_from("<I", s, 36)[0]
        secs.append((s[:8].rstrip(b"\0").decode(errors="replace"), va, vs, ro, rs, ch))
    img = struct.unpack_from("<I", d, e+24+56)[0]
    return d, secs, img
def r2o(secs, r):
    for n, va, vs, ro, rs, ch in secs:
        if va <= r < va+rs: return ro + r - va
def o2r(secs, o):
    for n, va, vs, ro, rs, ch in secs:
        if ro <= o < ro+rs: return va + o - ro
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); md.detail = True
def make_pattern(d, secs, rva, ninstr=24):
    o = r2o(secs, rva); pat = b""
    for ins in md.disasm(d[o:o+ninstr*15], 0):
        b = bytes(ins.bytes); mask = bytearray(b)
        wild = False
        # rip-relative memory operand or rel32 branch: wildcard the displacement/immediate
        if ins.id in (capstone.x86.X86_INS_CALL, capstone.x86.X86_INS_JMP) or ins.mnemonic.startswith("j"):
            if len(b) >= 5 and b[0] in (0xE8, 0xE9): pat += re.escape(b[:1]) + b"...."; wild = True
        if not wild and ins.disp_offset and any(op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP for op in ins.operands):
            pat += re.escape(b[:ins.disp_offset]) + b"...." + re.escape(b[ins.disp_offset+4:]); wild = True
        if not wild: pat += re.escape(b)
        if len(pat) > 0 and ins.mnemonic == "ret": break
        if ninstr and ins.address >= ninstr*15: break
        ninstr -= 1
        if ninstr == 0: break
    return re.compile(pat, re.S)
def find_all(d, secs, pat):
    hits = []
    for n, va, vs, ro, rs, ch in secs:
        if not (ch & 0x20000000): continue
        for m in pat.finditer(d, ro, ro+rs): hits.append(va + m.start() - ro)
    return hits
dO, sO, imgO = load(OLD); dN, sN, imgN = load(NEW)
print("old SizeOfImage 0x%x, new SizeOfImage 0x%x" % (imgO, imgN))
targets = {"createSceneObjectFrom": 0x3A6E600, "setWorldTransform": 0x261AF00, "setEnable": 0x261B9A0,
           "StringDataAlloc": 0x1391FD0, "PrefabPathCtor": 0x1319EA0, "PathNormalizeCtor": 0x1238AB0, "attachChild": 0x2621DC0,
           "getWorldTransform": 0x2619D00, "sectorLoader": 0x8590C0, "StaticStringCtor": 0x1231110}
results = {}
for name, rva in targets.items():
    for n in (24, 16, 12, 40):
        pat = make_pattern(dO, sO, rva, n)
        selfcheck = find_all(dO, sO, pat)
        hits = find_all(dN, sN, pat)
        if len(hits) == 1 and rva in selfcheck:
            print("%-22s old 0x%07x -> new 0x%07x  (pattern %d instr, old self-hits %d)" % (name, rva, hits[0], n, len(selfcheck)))
            results[name] = hits[0]; break
    else:
        print("%-22s old 0x%07x -> NOT UNIQUE/FOUND (last try: %d hits)" % (name, rva, len(hits)))
# world global via the take-or-steal sequence: mov rax,[rip+d]; mov rcx,[rax+0x30]; mov rax,[rbx+0xa0]; cmp [rcx+0x58],rax
pat = re.compile(rb"\x48\x8b\x05(....)\x48\x8b\x48\x30\x48\x8b\x83\xa0\x00\x00\x00\x48\x39\x41\x58", re.S)
import collections
globs = collections.Counter()
for m in pat.finditer(dN):
    r = o2r(sN, m.start())
    if r: globs[r + 7 + struct.unpack("<i", m.group(1))[0]] += 1
print("world global candidates (new):", [(hex(k), v) for k, v in globs.most_common(3)])
results["WorldGlobal"] = globs.most_common(1)[0][0] if globs else None
import json; json.dump({k: v for k, v in results.items()}, open("notes/rebase_result.json", "w"), indent=1)
