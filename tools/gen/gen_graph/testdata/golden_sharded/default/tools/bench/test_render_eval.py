#!/usr/bin/env python3
"""Unit tests for render_eval.py."""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import render_eval as re  # noqa: E402


def _render_script(html: str) -> str:
    """Extract the app script (the one that reads `const D =`) from the HTML."""
    import re
    blocks = re.findall(r"<script>([\s\S]*?)</script>", html)
    for b in blocks:
        if "const D =" in b:
            return b
    return ""


def _balanced_js(code: str) -> bool:
    """Cheap paren/bracket/brace balance check (skips strings/comments)."""
    pairs = {")": "(", "]": "[", "}": "{"}
    stack = []
    i, n = 0, len(code)
    while i < n:
        c = code[i]
        if c == "/" and i + 1 < n and code[i + 1] == "/":
            i += 2
            while i < n and code[i] != "\n":
                i += 1
            continue
        if c == "/" and i + 1 < n and code[i + 1] == "*":
            i += 2
            while i + 1 < n and not (code[i] == "*" and code[i + 1] == "/"):
                i += 1
            i += 2
            continue
        if c in "\"'`":
            q = c
            i += 1
            while i < n:
                if code[i] == "\\":
                    i += 2
                    continue
                if code[i] == q:
                    i += 1
                    break
                i += 1
            continue
        if c in "([{":
            stack.append(c)
        elif c in ")]}":
            if not stack or stack[-1] != pairs[c]:
                return False
            stack.pop()
        i += 1
    return not stack


def _minimal_doc() -> dict:
    return {
        "mode": "inproc",
        "load": "baseline",
        "seconds": 5,
        "chain": ["camera", "perception", "fusion"],
        "nodes": [
            {"node": "camera", "lat_n": 10, "lat_p50": 5000, "lat_p95": 9000,
             "lat_p99": 12000, "lat_max": 15000, "work_n": 10, "work_p50": 1000,
             "work_p95": 2000, "work_max": 3000, "idle": 2},
        ],
        "edges": [
            {"channel": "image", "count": 10, "rate": 2.0, "lat_p50": 4000,
             "lat_p95": 8000, "lat_p99": 10000, "lat_max": 12000, "jit_std": 5.5},
        ],
        "e2e": {"n": 8, "broken": 0, "p50": 50000, "p95": 90000, "p99": 100000,
                "max": 120000, "series": [[0, 50000], [100, 90000]]},
        "node_series": {"camera": [[0, 5], [100, 9]]},
        "edge_series": {"image": [[0, 4], [100, 8]]},
        "work_series": {"camera": [[0, 1, 0, 6], [100, 2, 0, 6]]},
        "resources": [[0, 0, 8000], [200, 2, 8000]],
    }


class RenderEvalTests(unittest.TestCase):
    def test_render_contains_sections(self) -> None:
        html = re.render(_minimal_doc(), "/* echarts */")
        for marker in [
            "thyme pipeline performance report",
            "Communication delay per edge",
            "Compute time per node",
            "End-to-end latency",
            "Resource usage",
            "Overall conclusion",
            "image",
            "camera",
        ]:
            self.assertIn(marker, html)

    def test_render_empty_doc_does_not_crash(self) -> None:
        html = re.render({}, "/* echarts */")
        self.assertIn("thyme pipeline performance report", html)

    def test_render_script_is_valid_js(self) -> None:
        # Regression: a mismatched paren in the toggle handlers once broke the
        # whole <script> block and left every chart blank.
        html = re.render(_minimal_doc(), "/* echarts */")
        script = _render_script(html)
        self.assertIn("const D =", script)
        self.assertTrue(_balanced_js(script), "app script has unbalanced brackets")

    def test_normalize_resources_list_and_dict(self) -> None:
        self.assertEqual(re._normalize_resources([[0, 0, 8000]]),
                         {"self": [(0, 0, 8000)]})
        self.assertEqual(re._normalize_resources({"a": [[0, 0, 8000]]}),
                         {"a": [(0, 0, 8000)]})

    def test_cpu_series(self) -> None:
        cpu, rss = re._cpu_series([(0, 0, 8192), (1000, 10, 8192)])
        self.assertTrue(len(cpu) == 1)
        self.assertAlmostEqual(cpu[0][1], 10.0, places=1)  # 10 jiffies / 1s = 10%
        self.assertEqual(rss[0][1], 8.0)

    def test_main_roundtrip(self) -> None:
        script = Path(__file__).resolve().parent.parent / "render_eval.py"
        with tempfile.TemporaryDirectory() as tmp:
            in_path = Path(tmp) / "eval.json"
            out_path = Path(tmp) / "report.html"
            in_path.write_text(json.dumps(_minimal_doc()), encoding="utf-8")
            proc = subprocess.run(
                [sys.executable, str(script), "--in", str(in_path),
                 "--out", str(out_path)],
                capture_output=True, text=True, check=False,
            )
            self.assertEqual(proc.returncode, 0, proc.stderr)
            html = out_path.read_text(encoding="utf-8")
            self.assertIn("thyme pipeline performance report", html)

    def test_main_missing_input(self) -> None:
        script = Path(__file__).resolve().parent.parent / "render_eval.py"
        with tempfile.TemporaryDirectory() as tmp:
            proc = subprocess.run(
                [sys.executable, str(script), "--in", str(Path(tmp) / "nope.json")],
                capture_output=True, text=True, check=False,
            )
            self.assertEqual(proc.returncode, 2)


if __name__ == "__main__":
    unittest.main()
