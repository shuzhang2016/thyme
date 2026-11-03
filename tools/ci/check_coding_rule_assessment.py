#!/usr/bin/env python3
"""Validate and generate the local coding-rule evidence assessment."""
from __future__ import annotations

import argparse
import json
import os
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
MAPPING = "tools/ci/coding_rule_assessment.json"
REPORT = "docs/security/coding-standard-assessment.md"
CODING_RULES = "docs/reference/coding-rules.md"
CLANG_TIDY = ".clang-tidy"
RUN_CLANG_TIDY = "tools/ci/run_clang_tidy.py"
CI_FILE = ".github/workflows/ci.yml"


def _nonempty_string(value: object) -> bool:
    return isinstance(value, str) and bool(value.strip())


def _ci_job_block(ci_text: str, job: str) -> str | None:
    match = re.search(rf"(?m)^  {re.escape(job)}:\s*$", ci_text)
    if match is None:
        return None
    remainder = ci_text[match.end():]
    next_job = re.search(r"(?m)^  [A-Za-z0-9_-]+:\s*$", remainder)
    return remainder if next_job is None else remainder[:next_job.start()]


def ctest_names_from_json(path: pathlib.Path) -> set[str]:
    document = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(document, dict) or not isinstance(document.get("tests"), list):
        raise ValueError("CTest JSON has no tests list")
    names: set[str] = set()
    for test in document["tests"]:
        if not isinstance(test, dict) or not _nonempty_string(test.get("name")):
            raise ValueError("CTest JSON contains a test without a name")
        name = test["name"]
        if name in names:
            raise ValueError(f"duplicate CTest name '{name}'")
        names.add(name)
    return names


def ctest_names_from_build(build_dir: pathlib.Path) -> set[str]:
    proc = subprocess.run(
        ["ctest", "--test-dir", str(build_dir), "-N", "--show-only=json-v1"],
        capture_output=True, text=True, check=False)
    if proc.returncode != 0:
        raise RuntimeError((proc.stderr or proc.stdout).strip())
    temporary = build_dir / f".coding-rule-ctest-{os.getpid()}.json"
    temporary.write_text(proc.stdout, encoding="utf-8")
    try:
        return ctest_names_from_json(temporary)
    finally:
        temporary.unlink(missing_ok=True)


def _unique_object(pairs: list[tuple[str, object]]) -> dict:
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON key '{key}'")
        result[key] = value
    return result


def _read_json(path: pathlib.Path) -> object:
    return json.loads(path.read_text(encoding="utf-8"),
                      object_pairs_hook=_unique_object)


def _cell(value: object) -> str:
    return str(value).replace("|", "\\|").replace("\n", " ").strip()


def _document_link(path: str) -> str:
    if path.startswith("docs/"):
        suffix = path[len("docs/"):] if path.startswith("docs/") else path
        return f"[`{path}`](../{suffix})"
    return f"[`{path}`](../../{path})"


def _evidence_text(evidence: dict) -> str:
    kind = evidence["type"]
    if kind == "cmake_flag":
        return f"CMake `{evidence['flag']}` + CI `test-werror`"
    if kind == "ci":
        return f"MR `{evidence['job']}` (`{evidence['required_text']}`)"
    if kind == "clang_tidy":
        return f"clang-tidy `{evidence['check']}` / CI `check-clang-tidy`"
    if kind == "ctest":
        return f"CTest `{evidence['name']}`"
    if kind == "document":
        return _document_link(evidence["path"])
    raise ValueError(f"unsupported evidence type '{kind}'")


def validate(root: pathlib.Path, mapping: object,
             ctest_names: set[str]) -> tuple[list[str], dict | None]:
    errors: list[str] = []
    if not isinstance(mapping, dict) or mapping.get("schema") != 1:
        return ["mapping: schema must be 1"], None
    standards = mapping.get("standards")
    rules = mapping.get("rules")
    if not isinstance(standards, list) or not standards:
        return ["mapping: standards must be a non-empty list"], None
    if not isinstance(rules, list) or not rules:
        return ["mapping: rules must be a non-empty list"], None

    for standard in standards:
        if not isinstance(standard, dict):
            errors.append("mapping: every standard must be an object")
            continue
        if not _nonempty_string(standard.get("name")):
            errors.append("mapping: standard name must not be empty")
        if standard.get("status") != "not_assessed":
            errors.append("mapping: external standard status must remain not_assessed")
        if not _nonempty_string(standard.get("reason")):
            errors.append("mapping: every standard needs a non-empty reason")

    try:
        coding_rules = (root / CODING_RULES).read_text(encoding="utf-8")
        cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
        ci = (root / CI_FILE).read_text(encoding="utf-8")
        clang_tidy = (root / CLANG_TIDY).read_text(encoding="utf-8")
    except (OSError, UnicodeError) as exc:
        return [f"cannot load assessment inputs: {exc}"], None
    documented_rules = set(re.findall(r"(?m)^## (CR-[0-9]+)\b", coding_rules))
    mapped_rules: set[str] = set()

    for rule in rules:
        if not isinstance(rule, dict) or not _nonempty_string(rule.get("id")):
            errors.append("mapping: every rule needs a non-empty id")
            continue
        rule_id = rule["id"]
        if rule_id in mapped_rules:
            errors.append(f"mapping: duplicate rule '{rule_id}'")
        mapped_rules.add(rule_id)
        if rule_id not in documented_rules:
            errors.append(f"mapping: orphan rule '{rule_id}'")
        if not _nonempty_string(rule.get("claim")):
            errors.append(f"{rule_id}: claim must not be empty")
        if not _nonempty_string(rule.get("limit")):
            errors.append(f"{rule_id}: evidence limit must not be empty")
        evidence_list = rule.get("evidence")
        if not isinstance(evidence_list, list) or not evidence_list:
            errors.append(f"{rule_id}: empty evidence")
            continue
        seen_evidence: set[str] = set()
        for evidence in evidence_list:
            if not isinstance(evidence, dict):
                errors.append(f"{rule_id}: evidence entries must be objects")
                continue
            key = json.dumps(evidence, sort_keys=True, ensure_ascii=False)
            if key in seen_evidence:
                errors.append(f"{rule_id}: duplicate evidence")
            seen_evidence.add(key)
            kind = evidence.get("type")
            if kind == "cmake_flag":
                flag = evidence.get("flag")
                if not _nonempty_string(flag) or flag not in cmake:
                    errors.append(f"{rule_id}: CMake flag is absent: '{flag}'")
            elif kind == "ci":
                job = evidence.get("job")
                required = evidence.get("required_text")
                block = _ci_job_block(ci, job) if isinstance(job, str) else None
                if block is None:
                    errors.append(f"{rule_id}: CI job does not exist: '{job}'")
                elif not _nonempty_string(required) or required not in block:
                    errors.append(f"{rule_id}: CI job '{job}' lacks '{required}'")
            elif kind == "clang_tidy":
                check = evidence.get("check")
                if not _nonempty_string(check) or check not in clang_tidy:
                    errors.append(f"{rule_id}: clang-tidy check is absent: '{check}'")
                block = _ci_job_block(ci, "check-clang-tidy")
                if block is None or RUN_CLANG_TIDY not in block:
                    errors.append(f"{rule_id}: check-clang-tidy does not invoke {RUN_CLANG_TIDY}")
            elif kind == "ctest":
                name = evidence.get("name")
                if not _nonempty_string(name) or name not in ctest_names:
                    errors.append(f"{rule_id}: referenced CTest does not exist: '{name}'")
            elif kind == "document":
                rel = evidence.get("path")
                if not _nonempty_string(rel):
                    errors.append(f"{rule_id}: document path must not be empty")
                    continue
                path = pathlib.PurePosixPath(rel)
                if path.is_absolute() or ".." in path.parts or not (root / path).is_file():
                    errors.append(f"{rule_id}: document does not exist inside repository: '{rel}'")
            else:
                errors.append(f"{rule_id}: unsupported evidence type '{kind}'")

    for rule_id in sorted(documented_rules - mapped_rules):
        errors.append(f"mapping: missing rule '{rule_id}'")
    context = {"standards": standards, "rules": rules}
    return errors, context


def render_report(context: dict) -> str:
    lines = [
        "# Coding standard and static analysis assessment",
        "",
        "> This file is generated by `tools/ci/check_coding_rule_assessment.py` from a machine-readable mapping. Do not edit it by hand.",
        "",
        "## Conclusion and scope",
        "",
        "This report assesses the engineering evidence for the repository's custom `CR-*` rules; it is **not** a clause-by-clause MISRA C++ or AUTOSAR C++14",
        "compliance report. `not_assessed` means no formal clause-by-clause assessment has been performed; it does not mean compliant, and it does not prove non-compliance.",
        "Tool output, registered tests and human review are evidence of different strength; this report does not address ASIL/HARA and makes no safety-certification or submission-readiness claim.",
        "",
        "| External standard | Status | Reason |",
        "|---|---|---|",
    ]
    for standard in context["standards"]:
        lines.append(f"| {_cell(standard['name'])} | `{standard['status']}` | {_cell(standard['reason'])} |")
    lines.extend([
        "",
        "## Local rule–evidence mapping",
        "",
        "CTest evidence means the test is registered in the current build tree; CTest self-tests and the CI configuration are responsible for execution. CMake/CI/clang-tidy",
        "evidence means the corresponding configuration is still wired up; it does not mean that compilation or static analysis was re-run when this report was generated.",
        "",
        "| Rule | Local criterion | Registered evidence | Evidence boundary |",
        "|---|---|---|---|",
    ])
    for rule in context["rules"]:
        evidence = "<br>".join(_evidence_text(item) for item in rule["evidence"])
        lines.append(f"| `{_cell(rule['id'])}` | {_cell(rule['claim'])} | {_cell(evidence)} | "
                     f"{_cell(rule['limit'])} |")
    lines.extend([
        "",
        "Formal MISRA/AUTOSAR work still requires: selecting the applicable standard version and the complete rule set, choosing and qualifying a checker, building a clause-by-clause",
        "compliant/deviation/not-applicable matrix, approving deviations, and having an independent role review the result. The repository does not currently present a local subset as a substitute for those artifacts.",
        "",
        "## Regeneration and verification",
        "",
        "```bash",
        "python3 tools/ci/check_coding_rule_assessment.py --build-dir build --write",
        "python3 tools/ci/check_coding_rule_assessment.py --build-dir build",
        "python3 tools/ci/test_check_coding_rule_assessment.py",
        "```",
        "",
    ])
    return "\n".join(lines)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=str(ROOT))
    parser.add_argument("--build-dir", default="build")
    parser.add_argument("--ctest-json")
    parser.add_argument("--write", action="store_true")
    args = parser.parse_args(argv)
    root = pathlib.Path(args.root).resolve()
    try:
        if args.ctest_json:
            names = ctest_names_from_json(pathlib.Path(args.ctest_json))
        else:
            names = ctest_names_from_build(pathlib.Path(args.build_dir))
        mapping = _read_json(root / MAPPING)
    except (OSError, RuntimeError, ValueError, json.JSONDecodeError) as exc:
        print(f"coding-rule assessment: cannot load inputs: {exc}", file=sys.stderr)
        return 1
    errors, context = validate(root, mapping, names)
    report_path = root / REPORT
    if not errors and context is not None:
        expected = render_report(context)
        if args.write:
            try:
                report_path.parent.mkdir(parents=True, exist_ok=True)
                report_path.write_text(expected, encoding="utf-8")
            except OSError as exc:
                errors.append(f"cannot write {REPORT}: {exc}")
        else:
            try:
                actual = report_path.read_text(encoding="utf-8")
            except OSError as exc:
                errors.append(f"cannot read {REPORT}: {exc}")
            else:
                if actual != expected:
                    errors.append(f"{REPORT}: stale report; rerun with --write")
    if errors:
        print("coding-rule assessment check failed:", file=sys.stderr)
        print("\n".join(errors), file=sys.stderr)
        return 1
    action = "generated" if args.write else "OK"
    print(f"coding-rule assessment {action} ({len(context['rules'])} local rules); "
          "external MISRA/AUTOSAR assessment remains separate")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
