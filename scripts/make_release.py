# Build cdmodkit and assemble a Nexus-ready zip.
#   python scripts/make_release.py 0.36            -> release/WorldBuilder-v0.36.zip (+ folder)
#   python scripts/make_release.py 0.36 --no-build -> package the existing build
# Steps: bump version strings in the sources, run build.bat, verify the .asi exports, copy plugin + data + docs + sdk,
# write THIRD_PARTY_NOTICES.md, zip. The game must not be running when you later copy the .asi into bin64.
import sys, re, os, shutil, subprocess, pathlib, datetime
ROOT = pathlib.Path(__file__).resolve().parent.parent
ASI = ROOT / "asi" / "cdmodkit"
REL = ROOT / "release"
if len(sys.argv) < 2: print(__doc__); sys.exit(1)
ver = sys.argv[1].lstrip("v"); build = "--no-build" not in sys.argv

# 1) version strings
for f, pat in [(ASI / "cdmodkit.cpp", r'cdmodkit\.asi v[0-9.]+ attached'), (ASI / "editor.cpp", r'World Builder  v[0-9.]+')]:
    s = f.read_text(encoding="utf-8")
    s2 = re.sub(pat, lambda m: re.sub(r'v[0-9.]+', 'v' + ver, m.group(0)), s)
    if s2 != s: f.write_text(s2, encoding="utf-8"); print("version ->", f.name)

# 2) build
if build:
    r = subprocess.run(["cmd", "/c", str(ASI / "build.bat")], capture_output=True, text=True, encoding="utf-8", errors="ignore")
    errs = [l for l in (r.stdout + r.stderr).splitlines() if " error " in l or "fatal" in l]
    if errs or not (ASI / "build" / "cdmodkit.asi").exists(): print("\n".join(errs) or "build failed"); sys.exit(2)
    print("build ok")

# 3) verify exports
try:
    import pefile
    pe = pefile.PE(str(ASI / "build" / "cdmodkit.asi"))
    names = sorted(e.name.decode() for e in pe.DIRECTORY_ENTRY_EXPORT.symbols)
    assert "cdk_spawn" in names, names
    print("exports:", ", ".join(names))
except ImportError:
    print("pefile not installed, skipping export check")

# 4) assemble
out = REL / f"WorldBuilder-v{ver}"
if out.exists(): shutil.rmtree(out)
(out / "bin64" / "cdmodkit").mkdir(parents=True)
(out / "sdk").mkdir()
shutil.copy(ASI / "build" / "cdmodkit.asi", out / "bin64")
for name in ("prefabs.tsv", "settings.txt", "errnames.txt"):
    shutil.copy(ASI / "data" / name, out / "bin64" / "cdmodkit")
# thumbnails and prefab_size.tsv are NOT shipped: the mod renders them locally from the player's own pack files
shutil.copy(ASI / "cdmodkit_api.h", out / "sdk")
shutil.copy(REL / "README.md", out)
shutil.copy(ROOT / "THIRD_PARTY_NOTICES.md", out)
shutil.copy(ROOT / "LICENSE", out)
zip_path = REL / f"WorldBuilder-v{ver}"
if (REL / f"WorldBuilder-v{ver}.zip").exists(): (REL / f"WorldBuilder-v{ver}.zip").unlink()
shutil.make_archive(str(zip_path), "zip", out)
size = os.path.getsize(f"{zip_path}.zip")
print(f"release: {zip_path}.zip ({size/1e6:.2f} MB)")
print("next: copy bin64\\cdmodkit.asi + bin64\\cdmodkit\\ into the game to test, then upload the zip to Nexus and tag the git commit")
