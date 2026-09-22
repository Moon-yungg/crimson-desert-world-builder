# Offline prefab thumbnails: prefab -> child SceneObjects + MeshGroup mesh instances -> .pam geometry (CDMW parser)
# -> flat-shaded software render (PIL polygons, painter's algorithm) -> data/thumbs/<fnv64(path)>.png (256x256, RGBA).
# Also writes data/prefab_size.tsv with bounding box dimensions.  Usage: python render_thumbs.py [--limit N] [--filter text] [--workers N]
import sys, os, math, time, pickle, struct, json, hashlib, argparse, traceback
from pathlib import Path
ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools" / "CDMW-Full")); sys.path.insert(0, str(ROOT / "scripts"))
import numpy as np
from PIL import Image, ImageDraw

def fnv64(s: str) -> str:
    h = 0xcbf29ce484222325
    for b in s.encode("utf-8"): h = ((h ^ b) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return "%016x" % h

def quat_to_mat(q):
    x, y, z, w = q
    return np.array([[1-2*(y*y+z*z), 2*(x*y-z*w), 2*(x*z+y*w)],
                     [2*(x*y+z*w), 1-2*(x*x+z*z), 2*(y*z-x*w)],
                     [2*(x*z-y*w), 2*(y*z+x*w), 1-2*(x*x+y*y)]], dtype=np.float32)

def collect_instances(obj, parent_m, parent_t, out):
    """walk SceneObject tree; each node has optional _worldTransform (scale3, quat4, pos3) relative to parent."""
    if isinstance(obj, dict):
        m, t = parent_m, parent_t
        xf = obj.get("_worldTransform")
        if isinstance(xf, (list, tuple)) and len(xf) >= 10:
            s = np.array(xf[0:3], dtype=np.float32); r = quat_to_mat(xf[3:7]); p = np.array(xf[7:10], dtype=np.float32)
            local = r * s[None, :]
            m = parent_m @ local; t = parent_m @ p + parent_t
        path = obj.get("_path")
        if isinstance(path, str) and path.endswith((".pami", ".pam")):
            out.append((path, m, t))
        for k, v in obj.items():
            if isinstance(v, (dict, list)): collect_instances(v, m, t, out)
    elif isinstance(obj, list):
        for v in obj: collect_instances(v, parent_m, parent_t, out)

_mesh_cache = {}
_pami_cache = {}
def resolve_pam(ctx, path):
    """.pami is XML: <StaticMesh Path="....pam"/> (variants like _snow reuse a base mesh with other materials)."""
    if path.endswith(".pam"): return path
    if path in _pami_cache: return _pami_cache[path]
    import re
    res = path.rsplit(".", 1)[0] + ".pam"
    d = ctx.get_file(path)
    if d:
        m = re.search(rb'<StaticMesh Path="([^"]+\.pam)"', d)
        if m: res = m.group(1).decode()
    _pami_cache[path] = res
    return res
def load_mesh(ctx, path):
    path = resolve_pam(ctx, path)
    if path in _mesh_cache: return _mesh_cache[path]
    from cdmw.modding.mesh_parser import parse_pam
    data = ctx.get_file(path)
    res = None
    if data and data[:4] == b"PAR ":
        try:
            pm = parse_pam(data, path)
            verts = []; faces = []; base = 0
            for sm in pm.submeshes:
                v = np.array([[vv.x, vv.y, vv.z] if hasattr(vv, "x") else list(vv)[:3] for vv in sm.vertices], dtype=np.float32)
                f = np.array([list(ff)[:3] for ff in sm.faces], dtype=np.int64) + base
                verts.append(v); faces.append(f); base += len(v)
            if verts: res = (np.concatenate(verts), np.concatenate(faces))
        except Exception: res = None
    _mesh_cache[path] = res
    return res

def render(verts, faces, size=256, max_faces=40000):
    if len(faces) > max_faces:
        sel = np.random.default_rng(0).choice(len(faces), max_faces, replace=False); faces = faces[sel]
    # camera: rotate so we look from front-right-above (azimuth 35 deg, elevation 25 deg)
    az, el = math.radians(35), math.radians(25)
    ry = np.array([[math.cos(az), 0, math.sin(az)], [0, 1, 0], [-math.sin(az), 0, math.cos(az)]], dtype=np.float32)
    rx = np.array([[1, 0, 0], [0, math.cos(el), -math.sin(el)], [0, math.sin(el), math.cos(el)]], dtype=np.float32)
    v = verts @ ry.T @ rx.T
    mn, mx = v.min(0), v.max(0); ext = float(max(mx[0]-mn[0], mx[1]-mn[1])) or 1.0
    scale = (size * 0.86) / ext
    cx, cy = (mn[0]+mx[0]) / 2, (mn[1]+mx[1]) / 2
    sx = (v[:, 0] - cx) * scale + size / 2; sy = size / 2 - (v[:, 1] - cy) * scale
    tri = v[faces]                                   # (n,3,3)
    n = np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0])
    ln = np.linalg.norm(n, axis=1); ok = ln > 1e-9; n = n[ok] / ln[ok][:, None]; faces = faces[ok]; tri = tri[ok]
    light = np.array([0.35, 0.8, 0.55], dtype=np.float32); light /= np.linalg.norm(light)
    shade = np.clip(np.abs(n @ light), 0, 1) * 0.75 + 0.25
    depth = tri[:, :, 2].mean(1)
    order = np.argsort(depth)                        # painter's: far (negative z = away?) first; we look down -z after rotation
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0)); d = ImageDraw.Draw(img)
    base = np.array([214, 196, 168], dtype=np.float32)
    for i in order:
        f = faces[i]; c = (base * shade[i]).astype(int)
        d.polygon([(float(sx[f[0]]), float(sy[f[0]])), (float(sx[f[1]]), float(sy[f[1]])), (float(sx[f[2]]), float(sy[f[2]]))], fill=(int(c[0]), int(c[1]), int(c[2]), 255))
    return img

def build_prefab_geometry(ctx, logical_path):
    import parse_parc
    from bier.EndianedBinaryIO import EndianedBytesIO
    from pycrimson._reflection import ReflectionParser
    phys = logical_path.lstrip("/")
    parts = phys.split("/", 1); phys_bin = parts[0] + "/bin__/" + parts[1]
    data = ctx.get_file(phys_bin) or ctx.get_file(phys)
    if not data: return None, "prefab missing"
    if data[:4] == b"PARC": chunks = parse_parc.parse_chunks(data)
    else: r = ReflectionParser(EndianedBytesIO(data), False); chunks = [{"objects": r.objects}]
    inst = []
    for c in chunks: collect_instances(c["objects"], np.eye(3, dtype=np.float32), np.zeros(3, dtype=np.float32), inst)
    if not inst: return None, "no meshes"
    verts = []; faces = []; base = 0
    for path, m, t in inst[:400]:
        mesh = load_mesh(ctx, path)
        if not mesh: continue
        v, f = mesh
        verts.append(v @ m.T + t); faces.append(f + base); base += len(v)
    if not verts: return None, "no geometry"
    return (np.concatenate(verts), np.concatenate(faces)), None

def main():
    ap = argparse.ArgumentParser(); ap.add_argument("--limit", type=int, default=0); ap.add_argument("--filter", default=""); ap.add_argument("--only", default="/object/")
    ap.add_argument("--force", action="store_true"); a = ap.parse_args()
    ctx = pickle.load(open(ROOT / "notes" / "packctx.pkl", "rb"))
    out_dir = ROOT / "asi" / "cdmodkit" / "data" / "thumbs"; out_dir.mkdir(parents=True, exist_ok=True)
    paths = [l.split("\t")[0] for l in open(ROOT / "asi" / "cdmodkit" / "data" / "prefabs.tsv", encoding="utf-8") if not l.startswith("#")]
    paths = [p for p in paths if p.startswith(a.only) and (a.filter in p)]
    if a.limit: paths = paths[:a.limit]
    sizes = {}
    sz_path = ROOT / "asi" / "cdmodkit" / "data" / "prefab_size.tsv"
    if sz_path.exists():
        for l in open(sz_path, encoding="utf-8"):
            c = l.rstrip("\n").split("\t")
            if len(c) >= 4: sizes[c[0]] = c[1:4]
    t0 = time.time(); done = skipped = failed = 0; reasons = {}
    for i, p in enumerate(paths):
        png = out_dir / (fnv64(p) + ".png")
        if png.exists() and not a.force and p in sizes: skipped += 1; continue
        try:
            geo, why = build_prefab_geometry(ctx, p)
            if geo is None: failed += 1; reasons[why] = reasons.get(why, 0) + 1; continue
            v, f = geo
            mn, mx = v.min(0), v.max(0); sizes[p] = ["%.2f" % (mx[0]-mn[0]), "%.2f" % (mx[1]-mn[1]), "%.2f" % (mx[2]-mn[2])]
            render(v, f).save(png); done += 1
        except Exception as e:
            failed += 1; reasons[type(e).__name__] = reasons.get(type(e).__name__, 0) + 1
        if (i + 1) % 200 == 0:
            print(f"{i+1}/{len(paths)} done={done} skipped={skipped} failed={failed} {time.time()-t0:.0f}s", flush=True)
            with open(sz_path, "w", encoding="utf-8") as fo:
                for k, vv in sizes.items(): fo.write(k + "\t" + "\t".join(vv) + "\n")
    with open(sz_path, "w", encoding="utf-8") as fo:
        for k, vv in sizes.items(): fo.write(k + "\t" + "\t".join(vv) + "\n")
    print(f"finished: done={done} skipped={skipped} failed={failed} in {time.time()-t0:.0f}s; reasons={reasons}")

if __name__ == "__main__": main()
