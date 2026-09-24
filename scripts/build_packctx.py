# Rebuild notes/packctx.pkl (pycrimson PackageContext over the installed game's pack indexes).
# Needed after every game patch: the pickled offsets point into the old .paz layout, reads then fail with LZ4 errors.
#   python scripts\build_packctx.py ["D:\SteamLibrary\steamapps\common\Crimson Desert"]
import pickle, sys, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools" / "pycrimson" / "src"))
from pycrimson._context import PackageContext

game = Path(sys.argv[1] if len(sys.argv) > 1 else r"D:\SteamLibrary\steamapps\common\Crimson Desert")
out = Path(__file__).resolve().parent.parent / "notes" / "packctx.pkl"
t0 = time.time(); ctx = PackageContext(game)
ctx._paz_handle_cache = {}   # open file handles must not be pickled
pickle.dump(ctx, open(out, "wb"))
print(f"{len(ctx._packs)} pack groups -> {out} ({out.stat().st_size / 1e6:.1f} MB) in {time.time() - t0:.0f}s")
