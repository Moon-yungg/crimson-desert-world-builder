# Generate wildcard byte signatures (IDA style) for the plugin from the OLD build and verify uniqueness in OLD and NEW.
import sys, re, struct, json, capstone
sys.argv = ["x"]
exec(open("scripts/rebase.py").read().split("dO, sO, imgO = load(OLD)")[0])   # reuse helpers
dO, sO, imgO = load(OLD); dN, sN, imgN = load(NEW)
res = json.load(open("notes/rebase_result.json"))
def sig_from(d, secs, rva, ninstr):
    o = r2o(secs, rva); out = []
    for ins in md.disasm(d[o:o+ninstr*15], 0):
        b = bytes(ins.bytes); parts = ["%02X" % x for x in b]
        if b[0] in (0xE8, 0xE9) and len(b) == 5: parts = [parts[0]] + ["??"]*4
        elif ins.disp_offset and any(op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP for op in ins.operands):
            for k in range(ins.disp_offset, ins.disp_offset+4): parts[k] = "??"
        out += parts
        ninstr -= 1
        if ninstr == 0 or ins.mnemonic == "ret": break
    return " ".join(out)
def count(d, secs, sig):
    toks = sig.split(); pat = b"".join(b"." if t == "??" else re.escape(bytes([int(t, 16)])) for t in toks)
    rx = re.compile(pat, re.S); n = 0; first = None
    for nm, va, vs, ro, rs, ch in secs:
        if not (ch & 0x20000000): continue
        for m in rx.finditer(d, ro, ro+rs):
            n += 1; first = first or (va + m.start() - ro)
    return n, first
old = {"setWorldTransform": 0x261AF00, "setEnable": 0x261B9A0, "StringDataAlloc": 0x1391FD0, "PrefabPathCtor": 0x1319EA0, "PathNormalizeCtor": 0x1238AB0}
sigs = {}
for name, rva in old.items():
    for n in (10, 14, 18, 24, 32):
        s = sig_from(dO, sO, rva, n)
        cO, fO = count(dO, sO, s); cN, fN = count(dN, sN, s)
        if cO == 1 and cN == 1 and fN == res[name]:
            print("%-18s %d instr: unique in both (new 0x%x)\n    %s" % (name, n, fN, s)); sigs[name] = s; break
    else: print("%-18s FAILED" % name)
# world global pattern
wg = "48 8B 05 ?? ?? ?? ?? 48 8B 48 30 48 8B 83 A0 00 00 00 48 39 41 58"
print("world-global pattern hits old/new:", count(dO, sO, wg)[0], count(dN, sN, wg)[0])
sigs["WorldGlobalRef"] = wg
json.dump(sigs, open("notes/sigs.json", "w"), indent=1)
