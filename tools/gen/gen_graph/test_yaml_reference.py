#!/usr/bin/env python3

import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile

SKIP_RETURN_CODE = 77
# Set by --require-deps (CI reference-parser job): missing PyYAML fails instead
# of skipping, so the job cannot pass without running the comparison.
REQUIRE_DEPS = False
ADMISSION_NEGATIVE_FIXTURES = {
    "lossless_insufficient",
    "lossless_unknown_wcet",
    "unknown_pool",
}
UNSUPPORTED_CONSTRUCTS = {
    "yaml_anchor",
    "yaml_alias",
    "yaml_block_scalar",
    "yaml_flow_list",
    "yaml_flow_map",
}
VALID_FIXTURES = [
    "scaffold/graph.yaml",
    "testdata/ports_tpl/graph.yaml",
    "testdata/ports_tpl/graph_bad.yaml",
    "testdata/minimal_norecord/graph.yaml",
    "testdata/rate/dummy_twin.yaml",
    "testdata/rate/product_cyclic.yaml",
    "testdata/rate/depth_shallow.yaml",
    "testdata/layout_tpl/graph.yaml",
    "testdata/event_tpl/graph.yaml",
    "testdata/event_tpl/graph_shm_queue.yaml",
    "testdata/event_tpl/graph_depth_queue.yaml",
    "testdata/event_tpl/graph_depth_notify.yaml",
    "testdata/event_tpl/graph_known_event.yaml",
    "testdata/event_tpl/graph_sched.yaml",
    "testdata/eval_chains/graph.yaml",
    "testdata/golden_cyclic/default/graph.yaml",
    "testdata/broadcast_tpl/graph.yaml",
    "testdata/derive_tpl/graph.yaml",
    "testdata/cycle/feedback.yaml",
]
EXTRA_NEGATIVE_FIXTURES = [
    "testdata/cycle/cycle.yaml",
    "testdata/cycle/feedback_no_delay.yaml",
]


def reference_parse(path):
    import yaml

    try:
        list(yaml.parse(path.read_text(encoding="utf-8")))
        return True, ""
    except (UnicodeError, yaml.YAMLError) as error:
        return False, str(error)


def gen_parse(gen, path, output_dir=None):
    if output_dir is None:
        command = [gen, "--in", path, "--emit-json"]
    else:
        command = [gen, "--in", path, "--out", output_dir, "--update", "generated"]
    result = subprocess.run(command, text=True, capture_output=True, check=False)
    return result.returncode, result.stdout, result.stderr


def negative_cases(case_dir):
    rows = {}
    for line in (case_dir / "cases.txt").read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        name, expected_rc, expected_line, diagnostic = line.split("|", 3)
        if expected_rc != "2":
            raise RuntimeError(f"{name}: negative corpus expects exit {expected_rc}, not 2")
        rows[name] = diagnostic
    fixtures = {path.stem for path in case_dir.glob("*.yaml")}
    if fixtures != set(rows):
        raise RuntimeError(
            f"negative corpus mismatch: fixtures={sorted(fixtures)}, cases={sorted(rows)}"
        )
    return rows


def main():
    global REQUIRE_DEPS
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gen", required=True, type=Path)
    parser.add_argument("--source-dir", required=True, type=Path)
    parser.add_argument("--require-deps", action="store_true",
                        help="fail instead of skipping when PyYAML is missing")
    args = parser.parse_args()
    REQUIRE_DEPS = args.require_deps

    try:
        import yaml  # noqa: F401
    except ImportError as error:
        if REQUIRE_DEPS:
            print(f"FAIL: PyYAML is required: {error}", file=sys.stderr)
            return 1
        print(f"SKIP: PyYAML reference parser is unavailable: {error}")
        return SKIP_RETURN_CODE

    source_dir = args.source_dir.resolve()
    gen = args.gen.resolve()
    valid_count = 0
    for relative in VALID_FIXTURES:
        path = source_dir / relative
        reference_ok, reference_error = reference_parse(path)
        if not reference_ok:
            raise RuntimeError(f"{relative}: PyYAML rejected gen_graph-valid fixture: {reference_error}")
        returncode, stdout, stderr = gen_parse(gen, path)
        if returncode != 0:
            raise RuntimeError(f"{relative}: gen_graph rejected valid fixture ({returncode}):\n{stderr}")
        json.loads(stdout)
        valid_count += 1

    case_dir = source_dir / "testdata/negative"
    cases = negative_cases(case_dir)
    negative_count = 0
    unsupported_count = 0
    reference_reject_count = 0
    semantic_reject_count = 0
    negative_paths = [(name, case_dir / f"{name}.yaml") for name in sorted(cases)]
    negative_paths.extend((Path(relative).stem, source_dir / relative) for relative in EXTRA_NEGATIVE_FIXTURES)
    for name, path in negative_paths:
        reference_ok, reference_error = reference_parse(path)
        if name in ADMISSION_NEGATIVE_FIXTURES:
            with tempfile.TemporaryDirectory(prefix=f"thyme-{name}-") as output_dir:
                returncode, stdout, stderr = gen_parse(gen, path, output_dir)
        else:
            returncode, stdout, stderr = gen_parse(gen, path)
        diagnostic = stdout + stderr
        if returncode != 2:
            raise RuntimeError(f"{name}: gen_graph exit {returncode}, expected 2:\n{diagnostic}")
        if name in cases and cases[name] not in diagnostic:
            raise RuntimeError(f"{name}: missing pinned diagnostic {cases[name]!r}:\n{diagnostic}")
        if name in UNSUPPORTED_CONSTRUCTS:
            if not reference_ok:
                raise RuntimeError(f"{name}: PyYAML rejected unsupported-construct fixture: {reference_error}")
            if "LoadGraphYaml: unsupported YAML" not in diagnostic:
                raise RuntimeError(f"{name}: rejection is not explicitly an unsupported construct:\n{diagnostic}")
            unsupported_count += 1
        elif not reference_ok:
            reference_reject_count += 1
        else:
            if "LoadGraphYaml: unsupported YAML" in diagnostic:
                raise RuntimeError(f"{name}: semantic rejection mislabeled as unsupported YAML:\n{diagnostic}")
            if not diagnostic.strip():
                raise RuntimeError(f"{name}: gen_graph rejected valid YAML without a diagnostic")
            semantic_reject_count += 1
        negative_count += 1

    print(
        "ECO-12 gen_graph/PyYAML differential OK: "
        f"{valid_count} valid fixtures, {negative_count} negative fixtures "
        f"({unsupported_count} unsupported constructs, {reference_reject_count} invalid YAML, "
        f"{semantic_reject_count} semantic rejections); differences: 0"
    )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, ValueError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        sys.exit(1)
