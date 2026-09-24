# Add the character appearances (character/appearance/**/*.app_xml) to asi/cdmodkit/data/prefabs.tsv, one row each:
# path "/character/appearance/.../x.app_xml", tags "Appearance,<parts>", meshes 0, children = part count, mesh = body prefab.
# The game assembles a character at runtime from the prefabs an .app_xml lists (<Nude>, <Head>, <Hair>, <Armor>); the
# preview renderer does the same (thumbgen.cpp). Existing Appearance rows are replaced, prefab rows are left alone.
#   python scripts\build_appearance_index.py
import pickle, re, sys
from pathlib import Path
ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools" / "pycrimson" / "src"))
TSV = ROOT / "asi" / "cdmodkit" / "data" / "prefabs.tsv"
ctx = pickle.load(open(ROOT / "notes" / "packctx.pkl", "rb"))
apps = sorted(l.split("|", 1)[-1].strip() for l in open(ROOT / "notes" / "filelist.txt", encoding="utf-8", errors="ignore") if l.strip().endswith(".app_xml"))
raw = TSV.read_bytes(); crlf = b"\r\n" in raw[:4000]
lines = raw.decode("utf-8").splitlines()
keep = [l for l in lines if not (l.split("\t", 1)[0].endswith(".app_xml"))]
byname = {l.split("\t", 1)[0].rsplit("/", 1)[-1][:-7].lower() for l in keep if l.split("\t", 1)[0].endswith(".prefab")}
rows, unresolved, parts_total = [], 0, 0
for p in apps:
    try: s = ctx.get_file(p).decode("utf-8-sig", "replace")
    except Exception: continue
    groups = []; names = []
    for sec in ("Nude", "Head", "Hair", "Armor"):
        m = re.search(rf"<{sec}>(.*?)</{sec}>", s, re.S)
        n = re.findall(r'<Prefab Name="([^"]+)"', m.group(1)) if m else []
        if n: groups.append(sec if len(n) == 1 else f"{sec}:{len(n)}"); names += n
    if not names: continue
    parts_total += len(names); unresolved += sum(1 for n in names if n.lower() not in byname)
    body = names[0]
    rows.append(f"/{p}\tAppearance,{','.join(groups)}\t0\t{len(names)}\t{body}")
out = keep + rows
TSV.write_bytes(("\r\n" if crlf else "\n").join(out).encode("utf-8") + (b"\r\n" if crlf else b"\n"))
print(f"{len(rows)} appearances added ({len(apps)} files), {parts_total} parts, {unresolved} part names not in the prefab index")
