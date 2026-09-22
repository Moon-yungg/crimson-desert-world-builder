# Read memory of the running CrimsonDesert.exe (external, no restart needed). Usage: python peek.py chain <rva> <off1> <off2> ... | python peek.py rtti <addr>
import ctypes, ctypes.wintypes as w, struct, sys, subprocess, re
k32 = ctypes.windll.kernel32
PROCESS_VM_READ = 0x10; PROCESS_QUERY_INFORMATION = 0x400
def pid_of(name):
    out = subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {name}", "/FO", "CSV"], capture_output=True, text=True, encoding="utf-8", errors="ignore").stdout
    m = re.search(r'"%s","(\d+)"' % re.escape(name), out); return int(m.group(1)) if m else None
pid = pid_of("CrimsonDesert.exe")
if not pid: print("game not running"); sys.exit(1)
h = k32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, pid)
BASE = 0x140000000
def read(addr, n):
    buf = ctypes.create_string_buffer(n); got = ctypes.c_size_t()
    ok = k32.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, n, ctypes.byref(got))
    return buf.raw if ok and got.value == n else None
def q(addr):
    b = read(addr, 8); return struct.unpack("<Q", b)[0] if b else None
names = {}
for line in open("notes/rtti_static.txt", encoding="utf-8"):
    if line.startswith("#"): continue
    p = [x.strip() for x in line.split("|")]; names[int(p[2], 16)] = p[0]
def rtti(obj):
    vt = q(obj)
    if vt and BASE <= vt < BASE + 0x17000000: return names.get(vt - BASE, "vt@%x" % (vt - BASE))
    return None
def cstr(addr, n=200):
    b = read(addr, n)
    if not b: return None
    s = b.split(b"\0")[0]
    return s.decode(errors="replace") if s and all(32 <= c < 127 for c in s) else None
cmd = sys.argv[1]
if cmd == "chain":
    addr = BASE + int(sys.argv[2], 16); print("global 0x%x" % addr)
    cur = q(addr); print("  [global] = %s  rtti=%s" % (hex(cur) if cur else None, rtti(cur) if cur else None))
    for off in sys.argv[3:]:
        if not cur: break
        nxt = q(cur + int(off, 16))
        print("  [+%s] = %s  rtti=%s" % (off, hex(nxt) if nxt else None, rtti(nxt) if nxt else None))
        cur = nxt
elif cmd == "dump":
    addr = int(sys.argv[2], 16); n = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x100
    b = read(addr, n)
    for i in range(0, n, 8):
        v = struct.unpack_from("<Q", b, i)[0]; extra = ""
        if 0x10000 <= v < (1 << 47):
            r = rtti(v); s = cstr(v)
            if r: extra = "  -> " + r
            elif s: extra = "  -> str %r" % s
            elif BASE <= v < BASE + 0x17000000: extra = "  (img rva 0x%x)" % (v - BASE)
        print("  +0x%03x  %016x%s" % (i, v, extra))
