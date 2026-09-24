# Dump every game data table (gamedata/binarystaticinfo__/bin/<name>.staticinfoheader + .staticinfobody) to
# notes/staticinfo/<name>.tsv: row key, name (first string), row size, and every other readable string in the row.
# The rows are packed binary structs without a type table; field names live in the exe's reflection registration
# ("FactionInfo" + "_factionRelationGroupInfo", ...), types and order are not decoded yet. This is the readable first pass.
#   python scripts\build_packctx.py   (after a game patch)
#   python scripts\staticinfo_dump.py [table ...]
import os, pickle, re, struct, sys
from pathlib import Path
ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools" / "CDMW-Full")); sys.path.insert(0, str(ROOT / "tools" / "pycrimson" / "src"))
from cdmw.core.structured_binary_editor import parse_pabgh_table   # row directory: count + (key, offset) per row

BIN = "gamedata/binarystaticinfo__/bin/"
ctx = pickle.load(open(ROOT / "notes" / "packctx.pkl", "rb"))
out = ROOT / "notes" / "staticinfo"; out.mkdir(exist_ok=True)
names = sys.argv[1:] or sorted({l.split("|", 1)[-1].strip()[len(BIN):-len(".staticinfobody")]
                                for l in open(ROOT / "notes" / "filelist.txt", encoding="utf-8", errors="ignore")
                                if l.split("|", 1)[-1].strip().startswith(BIN) and l.strip().endswith(".staticinfobody")})

def strings(rec):   # u32 length + printable ASCII/UTF-8 (names, paths, keys as text); Korean dev notes are kept as UTF-8
    i, found = 0, []
    while i + 4 <= len(rec):
        n = struct.unpack_from("<I", rec, i)[0]
        if 1 <= n <= 400 and i + 4 + n <= len(rec):
            s = rec[i + 4:i + 4 + n]
            try: t = s.decode("utf-8")
            except UnicodeDecodeError: t = None
            if t and all(c.isprintable() for c in t) and (len(t) > 1 or t.isalnum()): found.append((i, t)); i += 4 + n; continue
        i += 1
    return found

summary = []
for name in names:
    try:
        h = ctx.get_file(BIN + name + ".staticinfoheader"); b = ctx.get_file(BIN + name + ".staticinfobody")
        t = parse_pabgh_table(h, payload=b)
    except Exception as e:
        summary.append(f"{name}\tERROR {e}"); continue
    rows = sorted(t.rows, key=lambda r: r.offset)
    with open(out / f"{name}.tsv", "w", encoding="utf-8", newline="\n") as f:
        f.write("key\tname\tbytes\tstrings (offset:text)\n")
        for k, r in enumerate(rows):
            end = rows[k + 1].offset if k + 1 < len(rows) else len(b)
            rec = b[r.offset:end]; st = strings(rec)
            nm = st[0][1] if st and st[0][0] <= 12 else ""
            rest = [f"{o}:{s}" for o, s in st if not (s == nm and o <= 12)]
            key = int.from_bytes(r.key, "little") if len(r.key) <= 8 else r.key.hex()
            f.write(f"{key}\t{nm}\t{len(rec)}\t" + " | ".join(x.replace("\t", " ").replace("\n", " ") for x in rest) + "\n")
    summary.append(f"{name}\t{len(rows)} rows\tkey {t.key_width} B\t{len(b)} B")
    print(summary[-1])
open(out / "_tables.tsv", "w", encoding="utf-8", newline="\n").write("\n".join(summary) + "\n")
