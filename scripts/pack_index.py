# Packs asi/cdmodkit/data/prefabs.tsv into asi/cdmodkit/build/prefabs.lz4 (8-byte header "CDK1" + u32 raw size, then one
# LZ4 block). The blob is linked into cdmodkit.asi as an RCDATA resource; the plugin writes bin64\cdmodkit\prefabs.tsv from it
# when the file is missing (mod managers that install only the .asi). Run by build.bat before compiling.
import struct, pathlib, sys
ROOT = pathlib.Path(__file__).resolve().parent.parent
src = ROOT / "asi" / "cdmodkit" / "data" / "prefabs.tsv"
dst = ROOT / "asi" / "cdmodkit" / "build" / "prefabs.lz4"
dst.parent.mkdir(exist_ok=True)
raw = src.read_bytes()
try:
    import lz4.block as lb
    comp = lb.compress(raw, mode="high_compression", store_size=False)
except ImportError:
    print("pack_index: python module lz4 missing (pip install lz4)"); sys.exit(1)
if dst.exists() and dst.read_bytes()[8:] == comp: print("pack_index: unchanged (%d -> %d bytes)" % (len(raw), len(comp))); sys.exit(0)
dst.write_bytes(b"CDK1" + struct.pack("<I", len(raw)) + comp)
print("pack_index: %d -> %d bytes -> %s" % (len(raw), len(comp), dst))
