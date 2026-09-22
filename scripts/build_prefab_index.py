# Build asi/cdmodkit/data/prefabs.tsv: logical path, tags (component kinds), mesh count, child count, first mesh name.
import sys, pickle, time, re, collections
sys.path.insert(0, "scripts")
import parse_parc
from bier.EndianedBinaryIO import EndianedBytesIO
from pycrimson._reflection import ReflectionParser
ctx = pickle.load(open("notes/packctx.pkl", "rb"))
paths = [l.strip() for l in open("asi/cdmodkit/data/prefabs.txt", encoding="utf-8") if l.strip()]
SKIP = {"SceneObject", "MeshGroupInfo"}
FAMILY = [("Houdini", "Houdini"), ("HDA", "Houdini"), ("CustomAttribute", None), ("Spline", "Spline"), ("Socket", "Socket"),
          ("SkinnedMesh", "SkinnedMesh"), ("MeshGroup", "Mesh"), ("EditorMesh", None), ("Mesh", "Mesh"), ("GimmickSpawnData", "Gimmick"),
          ("Gimmick", "Gimmick"), ("Light", "Light"), ("Decal", "Decal"), ("Effect", "Effect"), ("Tree", "Tree"), ("Cloth", "Cloth"),
          ("Sound", "Sound"), ("Water", "Water"), ("Physics", "Physics"), ("Collision", "Collision"), ("Navigation", "Navigation"),
          ("Alias", None), ("GameData", "GameData"), ("SceneObjectSocketReference", None), ("Animation", "Animation"), ("Camera", "Camera"),
          ("Trigger", "Trigger"), ("Character", "Character"), ("Impostor", None), ("Lod", None), ("LOD", None)]
def tagof(t):
    t = t.split("@")[0]
    if t.startswith("/"): return "SubPrefab"
    if t.startswith("ResourceReferencePath") or t in SKIP: return None
    t = re.sub(r"Component$", "", t)
    for pre, fam in FAMILY:
        if t.startswith(pre) or t.endswith(pre): return fam
    return "Other"
def walk(o, acc):
    if isinstance(o, dict):
        t = o.get("__pycr_type__")
        if t:
            if t == "SceneObject": acc["children"] += 1
            tag = tagof(t)
            if tag: acc["tags"][tag] += 1
        for k, v in o.items():
            if k == "_path" and isinstance(v, str) and v.endswith((".pami", ".pam")):
                acc["meshes"] += 1
                if not acc["mesh"]: acc["mesh"] = v.rsplit("/", 1)[-1].rsplit(".", 1)[0]
            walk(v, acc)
    elif isinstance(o, list):
        for v in o: walk(v, acc)
t0 = time.time(); out = open("asi/cdmodkit/data/prefabs.tsv", "w", encoding="utf-8"); errs = 0
out.write("#path\ttags\tmeshes\tchildren\tmesh\n")
for i, p in enumerate(paths):
    logical = p.replace("/bin__/", "/", 1)
    acc = {"tags": collections.Counter(), "meshes": 0, "children": 0, "mesh": ""}
    try:
        data = ctx.get_file(p.lstrip("/"))
        if data:
            if data[:4] == b"PARC": chunks = parse_parc.parse_chunks(data)
            else: r = ReflectionParser(EndianedBytesIO(data), False); chunks = [{"objects": r.objects}]
            for c in chunks: walk(c["objects"], acc)
            acc["children"] = max(0, acc["children"] - 1)
    except Exception as e:
        errs += 1; acc["tags"]["parse-error"] += 1
    tags = ",".join(f"{k}:{v}" if v > 1 else k for k, v in sorted(acc["tags"].items(), key=lambda kv: -kv[1]))
    out.write(f"{logical}\t{tags}\t{acc['meshes']}\t{acc['children']}\t{acc['mesh']}\n")
    if i % 5000 == 0: print(f"{i}/{len(paths)} {time.time()-t0:.0f}s errs={errs}", flush=True)
out.close(); print(f"done {len(paths)} in {time.time()-t0:.0f}s, parse errors {errs}")
