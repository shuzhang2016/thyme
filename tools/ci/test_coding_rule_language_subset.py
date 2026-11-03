#!/usr/bin/env python3
"""Prove that the core language-subset flags reject their targeted constructs."""
from __future__ import annotations

import argparse
import pathlib
import subprocess
import sys
import tempfile

CASES = {
    "throw": (
        "-fno-exceptions",
        "int main() { throw 1; }\n",
    ),
    "try_catch": (
        "-fno-exceptions",
        "int main() { try { return 0; } catch (...) { return 1; } }\n",
    ),
    "dynamic_cast": (
        "-fno-rtti",
        "struct Base { virtual ~Base() = default; };\n"
        "struct Derived : Base {};\n"
        "int main() { Derived d; Base* b = &d; "
        "return dynamic_cast<Derived*>(b) == nullptr; }\n",
    ),
    "typeid": (
        "-fno-rtti",
        "#include <typeinfo>\n"
        "int main() { return typeid(int) == typeid(long); }\n",
    ),
}


def compile_source(cxx: str, source: pathlib.Path, output: pathlib.Path,
                   flag: str | None) -> subprocess.CompletedProcess[str]:
    command = [cxx, "-std=c++17", "-c", str(source), "-o", str(output)]
    if flag is not None:
        command.append(flag)
    return subprocess.run(command, capture_output=True, text=True, check=False)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", required=True)
    args = parser.parse_args(argv)
    failures: list[str] = []
    with tempfile.TemporaryDirectory(prefix="thyme-language-subset-") as tmp:
        root = pathlib.Path(tmp)
        for name, (flag, text) in CASES.items():
            source = root / f"{name}.cpp"
            source.write_text(text, encoding="utf-8")
            positive = compile_source(args.cxx, source, root / f"{name}-positive.o", None)
            if positive.returncode != 0:
                failures.append(f"{name}: positive control failed without {flag}\n"
                                f"{positive.stdout}{positive.stderr}")
                continue
            negative = compile_source(args.cxx, source, root / f"{name}-negative.o", flag)
            if negative.returncode == 0:
                failures.append(f"{name}: {flag} unexpectedly accepted the forbidden construct")
    if failures:
        print("language-subset compile negatives failed:", file=sys.stderr)
        print("\n".join(failures), file=sys.stderr)
        return 1
    print(f"language-subset compile negatives OK ({len(CASES)} constructs, "
          "each with a positive control)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
