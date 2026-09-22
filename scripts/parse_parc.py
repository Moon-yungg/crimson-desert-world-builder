# Parse PARC container files (.palevel/.prefab/...) : every chunk = "PARC" + u16 kind + 10 zero bytes + reflection block
import sys, json, re
from pathlib import Path
from bier.EndianedBinaryIO import EndianedBytesIO
from pycrimson._reflection import ReflectionParser

CHUNK_RE = re.compile(rb"PARC(..)\x00{10}", re.S)

def find_chunks(data: bytes):
    out = []
    for m in CHUNK_RE.finditer(data):
        kind = int.from_bytes(m.group(1), "little")
        out.append((m.start(), kind))
    return out

def parse_chunks(data: bytes, debug=False):
    chunks = []
    for pos, kind in find_chunks(data):
        start = pos + 16
        if data[start:start+2] != b"\xff\xff":
            continue
        reader = EndianedBytesIO(data)
        reader.seek(start)
        r = ReflectionParser(reader, True, enable_debug_logging=debug)
        chunks.append({"chunk_offset": pos, "kind": kind, "types": [t.name for t in r._types],
                       "objects": r.objects, "_parser": r})
    return chunks

def parse_file(path: Path, debug=False):
    data = path.read_bytes()
    if data[:4] != b"PARC":
        reader = EndianedBytesIO(data)
        r = ReflectionParser(reader, False, enable_debug_logging=debug)
        return [{"chunk_offset": 0, "kind": None, "types": [t.name for t in r._types], "objects": r.objects, "_parser": r}]
    return parse_chunks(data, debug)

if __name__ == "__main__":
    p = Path(sys.argv[1]); out = Path(sys.argv[2]) if len(sys.argv) > 2 and not sys.argv[2].startswith("--") else p.with_suffix(".json")
    chunks = parse_file(p, debug="--debug" in sys.argv)
    for c in chunks:
        print(f"chunk @0x{c['chunk_offset']:x} kind=0x{c['kind']:04x} objects={len(c['objects'])} types={c['types']}" if c['kind'] is not None else f"objects={len(c['objects'])} types={c['types']}")
        if "--types" in sys.argv: c["_parser"].print_all_types()
    out.write_text(json.dumps([{k: v for k, v in c.items() if k != "_parser"} for c in chunks], indent=1, ensure_ascii=False))
    print("->", out)
