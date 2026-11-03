#!/usr/bin/env python3
"""CTest entry point: run the Python unittest suite with the C++ fixture."""

from __future__ import annotations

import os
from pathlib import Path
import sys
import unittest


def main() -> int:
    py_root = Path(__file__).resolve().parents[1]
    sys.path.insert(0, str(py_root))
    if len(sys.argv) > 1:
        os.environ["THYME_PY_FIXTURE"] = str(Path(sys.argv[1]).resolve())
    suite = unittest.defaultTestLoader.discover(
        str(Path(__file__).resolve().parent), pattern="test_*.py")
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
