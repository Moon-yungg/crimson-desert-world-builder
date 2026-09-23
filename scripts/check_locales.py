"""Validate the shipped UI translation resource and its printf placeholders."""
from pathlib import Path
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
    print(f"Locale pack OK: {count} entries, {len(EXPECTED)} languages, all placeholders match.")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError) as exc:
        print(f"Locale validation failed: {exc}", file=sys.stderr)
        sys.exit(1)
