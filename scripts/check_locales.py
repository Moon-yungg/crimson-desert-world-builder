"""Validate the shipped UI translation resource and its printf placeholders."""
from pathlib import Path
import ast
import re
import sys

ROOT = Path(__file__).resolve().parent.parent
PACK = ROOT / "asi" / "cdmodkit" / "data" / "locales.tsv"
EXPECTED = ["en", "zh-CN", "zh-TW", "de", "fr", "ko", "ja", "es", "pt-BR", "ru", "tr"]
LANGUAGE_NAMES = [
    "Auto (system)", "English", "Simplified Chinese", "Traditional Chinese", "German", "French",
    "Korean", "Japanese", "Spanish", "Portuguese (Brazil)", "Russian", "Turkish",
]
PRINTF = re.compile(r"%(?!%)(?:[-+#0 ]*(?:\d+|\*)?(?:\.(?:\d+|\*))?(?:hh|h|ll|l|I64|I32|z|t|j|L)?[diuoxXfFeEgGaAcspn])")
CPP_STRING = r'"(?:\\.|[^"\\])*"'
TRANSLATE_CALL = re.compile(r'\bT(?:Stable)?\(\s*(?:ICON_[A-Z0-9_]+\s*)?(' + CPP_STRING + r')\s*\)')


def strip_cpp_comments(source: str) -> str:
    """Remove C/C++ comments without touching comment markers inside string literals."""
    out = []
    i = 0
    quote = None
    while i < len(source):
        c = source[i]
        if quote:
            out.append(c)
            if c == "\\" and i + 1 < len(source):
                i += 1
                out.append(source[i])
            elif c == quote:
                quote = None
            i += 1
            continue
        if c in ('"', "'"):
            quote = c
            out.append(c)
            i += 1
            continue
        if source.startswith("//", i):
            nl = source.find("\n", i + 2)
            if nl < 0:
                break
            out.append("\n")
            i = nl + 1
            continue
        if source.startswith("/*", i):
            end = source.find("*/", i + 2)
            i = len(source) if end < 0 else end + 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


def visible_key(text: str) -> str:
    # Mirrors i18n::StripVisibleKey for source literals. Icon macros are outside the captured literal.
    text = text.strip(" \t")
    return text.split("##", 1)[0]


def unescape_pack(value: str) -> str:
    out = []
    i = 0
    while i < len(value):
        if value[i] == "\\" and i + 1 < len(value):
            n = value[i + 1]
            if n == "n": out.append("\n"); i += 2; continue
            if n == "r": out.append("\r"); i += 2; continue
            if n == "t": out.append("\t"); i += 2; continue
            if n == "\\": out.append("\\"); i += 2; continue
        out.append(value[i])
        i += 1
    return "".join(out)


def source_translation_keys() -> set[str]:
    keys = set()
    for path in (ROOT / "asi" / "cdmodkit").glob("*.cpp"):
        source = strip_cpp_comments(path.read_text(encoding="utf-8"))
        for match in TRANSLATE_CALL.finditer(source):
            try:
                text = ast.literal_eval(match.group(1))
            except (SyntaxError, ValueError):
                continue
            key = visible_key(text)
            if key:
                keys.add(key)
    return keys


def main() -> int:
    lines = PACK.read_text(encoding="utf-8").splitlines()
    if not lines:
        raise ValueError(f"empty locale pack: {PACK}")
    header = lines[0].split("\t")
    if header != ["key", *EXPECTED]:
        raise ValueError(f"unexpected locale columns: {header}")

    seen = set()
    count = 0
    for line_number, line in enumerate(lines[1:], 2):
        if not line:
            continue
        fields = line.split("\t")
        if len(fields) != len(header):
            raise ValueError(f"line {line_number}: expected 12 columns, got {len(fields)}")
        key, translations = fields[0], fields[1:]
        if not key:
            raise ValueError(f"line {line_number}: empty key")
        if key in seen:
            raise ValueError(f"line {line_number}: duplicate key {key!r}")
        if any(not value for value in translations):
            raise ValueError(f"line {line_number}: empty translation for {key!r}")
        if translations[0] != key:
            raise ValueError(f"line {line_number}: English value does not match key {key!r}")
        expected_tokens = PRINTF.findall(key)
        for language, value in zip(EXPECTED, translations):
            if PRINTF.findall(value) != expected_tokens:
                raise ValueError(f"line {line_number}: printf placeholders differ for {key!r} ({language})")
        seen.add(key)
        count += 1

    missing_names = [name for name in LANGUAGE_NAMES if name not in seen]
    if missing_names:
        raise ValueError(f"missing localized language names: {missing_names}")
    runtime_keys = {unescape_pack(key) for key in seen}
    missing_source = sorted(source_translation_keys() - runtime_keys)
    if missing_source:
        raise ValueError("source strings missing from locales.tsv:\n  " + "\n  ".join(repr(key) for key in missing_source))
    print(f"Locale pack OK: {count} entries, {len(EXPECTED)} languages, all placeholders match, all source T() keys present.")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError) as exc:
        print(f"Locale validation failed: {exc}", file=sys.stderr)
        sys.exit(1)
