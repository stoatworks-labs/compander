#!/usr/bin/env python3
"""Every #include must match its file's real case.

macOS is case-INSENSITIVE and Linux is not, so `#include "ofxsProcessing.H"`
against a file called `ofxsProcessing.h` compiles on macOS, compiles on Windows,
and fails only on the Linux OpenFX job — which runs after the tag.

That is exactly what happened cutting v0.1.0: three release jobs, two green, the
Linux one dead on a header that was not even needed, and no release published.
The macOS build could not have caught it and neither could any test.

    python3 tools/check-include-case.py

Exit code 1 means an include disagrees with the filename.
"""
import pathlib
import re
import sys

ROOTS = [
    pathlib.Path("source"),
    pathlib.Path("external/openfx/Support/include"),
    pathlib.Path("external/openfx/include"),
]


def main() -> int:
    # Every header the include paths can reach, indexed by lowercased name, so a
    # case-only difference is visible rather than resolved away by the OS.
    by_lower: dict[str, set[str]] = {}
    for root in ROOTS:
        if not root.exists():
            continue
        for f in root.rglob("*"):
            if f.is_file():
                by_lower.setdefault(f.name.lower(), set()).add(f.name)

    bad = []
    for src in pathlib.Path("source").rglob("*"):
        if src.suffix not in (".cpp", ".h", ".H"):
            continue
        for number, line in enumerate(src.read_text(errors="ignore").splitlines(), 1):
            m = re.match(r'\s*#\s*include\s*"([^"]+)"', line)
            if not m:
                continue
            name = pathlib.Path(m.group(1)).name
            real = by_lower.get(name.lower())
            if real and name not in real:
                bad.append((str(src), number, name, sorted(real)[0]))

    for src, number, wrote, actual in bad:
        print(f"  {src}:{number}: includes {wrote!r} but the file is {actual!r}")

    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
