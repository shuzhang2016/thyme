#!/usr/bin/env python3
"""gen_config_schema -- emit trigger configuration metadata from one schema.

ARCH-13 makes ``trigger_config.schema.json`` the single source for the
``[trigger_center]``, ``[recorder]``, ``[logging]``, ``[security]`` and
``[continuous]``, ``[chassis]`` and dynamic ``source.*`` / ``channel.*``
sections: key, status, reload
behaviour, type, default, owning contract and prose note. The generator emits
three artifacts from that one file:

  * ``include/thyme/record/trigger_config_schema.h`` -- C++ metadata table used
    by ``--check-config`` and the schema test;
  * the generated audit rows inside ``docs/guides/trigger-config.md``
    (DOC-08 tables for the keys this schema owns);
  * the generated schema-backed rows of the SIGHUP reload matrix in the same
    document.

The parser's value handling is a declared handwritten layer
(``parser_layer: handwritten`` in the schema). ``--check`` proves the
artifacts are byte-identical to a fresh generation **and** that every key
emitted by gen_graph ``trigger.base.ini`` goldens is in this schema (the
RecordManifest / generated record table joins the schema at ``record_channels``).
The C++ test proves the parser agrees with the table (samples parse, rejected
keys fail, defaults materialise, reload classification matches
``ReloadRequiresRestart``).

Usage:
  python3 tools/gen/config_schema/gen_config_schema.py
  python3 tools/gen/config_schema/gen_config_schema.py --check

Exit codes: 0 success, 2 schema/marker/usage error or stale artifact.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

EXIT_ERROR = 2

SCRIPT_DIR = Path(__file__).resolve().parent
DEFAULT_SCHEMA = SCRIPT_DIR / "trigger_config.schema.json"

HEADER_REL = Path("include/thyme/record/trigger_config_schema.h")
DOC_REL = Path("docs/guides/trigger-config.md")

AUDIT_BEGIN = "<!-- TRIGGER-CONFIG-SCHEMA-BEGIN"
AUDIT_END = "<!-- TRIGGER-CONFIG-SCHEMA-END -->"
AUDIT_MARKERS = {
    "trigger_center": (AUDIT_BEGIN, AUDIT_END),
    "recorder": ("<!-- TRIGGER-CONFIG-SCHEMA-RECORDER-BEGIN",
                 "<!-- TRIGGER-CONFIG-SCHEMA-RECORDER-END -->"),
    "logging": ("<!-- TRIGGER-CONFIG-SCHEMA-LOGGING-BEGIN",
                "<!-- TRIGGER-CONFIG-SCHEMA-LOGGING-END -->"),
    "security": ("<!-- TRIGGER-CONFIG-SCHEMA-SECURITY-BEGIN",
                 "<!-- TRIGGER-CONFIG-SCHEMA-SECURITY-END -->"),
    "continuous": ("<!-- TRIGGER-CONFIG-SCHEMA-CONTINUOUS-BEGIN",
                   "<!-- TRIGGER-CONFIG-SCHEMA-CONTINUOUS-END -->"),
    "chassis": ("<!-- TRIGGER-CONFIG-SCHEMA-CHASSIS-BEGIN",
                "<!-- TRIGGER-CONFIG-SCHEMA-CHASSIS-END -->"),
    "quota.*": ("<!-- TRIGGER-CONFIG-SCHEMA-QUOTA-BEGIN",
                "<!-- TRIGGER-CONFIG-SCHEMA-QUOTA-END -->"),
    "source.*": ("<!-- TRIGGER-CONFIG-SCHEMA-SOURCE-BEGIN",
                 "<!-- TRIGGER-CONFIG-SCHEMA-SOURCE-END -->"),
    "channel.*": ("<!-- TRIGGER-CONFIG-SCHEMA-CHANNEL-BEGIN",
                  "<!-- TRIGGER-CONFIG-SCHEMA-CHANNEL-END -->"),
}
SECTION_ARRAY = {
    "trigger_center": "kTriggerCenterSchema",
    "recorder": "kRecorderSchema",
    "logging": "kLoggingSchema",
    "security": "kSecuritySchema",
    "continuous": "kContinuousSchema",
    "chassis": "kChassisSchema",
    "quota.*": "kQuotaSchema",
    "source.*": "kSourceSchema",
    "channel.*": "kChannelSchema",
}
RELOAD_BEGIN = "<!-- TRIGGER-CONFIG-RELOAD-MATRIX-BEGIN"
RELOAD_END = "<!-- TRIGGER-CONFIG-RELOAD-MATRIX-END -->"

STATUS_CPP = {
    "implemented": "ConfigKeyStatus::kImplemented",
    "deprecated": "ConfigKeyStatus::kDeprecated",
    "rejected": "ConfigKeyStatus::kRejected",
    "future": "ConfigKeyStatus::kFuture",
}
RELOAD_CPP = {
    "hot": "ConfigKeyReload::kHot",
    "restart": "ConfigKeyReload::kRestartRequired",
    "startup-kept": "ConfigKeyReload::kStartupOnlyKept",
    "fixed": "ConfigKeyReload::kFixedAtStartup",
    "n/a": "ConfigKeyReload::kNotApplicable",
}
RELOAD_DOC = {
    "hot": "hot",
    "restart": "**restart required**",
    "startup-kept": "startup-only (kept silently)",
    "fixed": "fixed at startup",
    "n/a": "n/a (parse error)",
}
TYPES = {
    "path",
    "string",
    "bool",
    "uint32",
    "uint64",
    "int32",
    "enum",
    "source-name",
    "rejected",
}
TEXT_FIELDS = (
    "key",
    "type",
    "default",
    "sample",
    "note",
    "reload_note",
    "contract",
)


class SchemaError(Exception):
    pass


def _fail(message: str) -> None:
    raise SchemaError(message)


def _require_text(value: Any, path: str, *, allow_empty: bool = False) -> str:
    if not isinstance(value, str):
        _fail(f"{path}: expected a string")
    if not allow_empty and value == "":
        _fail(f"{path}: must not be empty")
    if "\n" in value or "|" in value:
        _fail(f"{path}: must not contain a newline or '|'")
    return value


def load_schema(path: Path) -> dict[str, Any]:
    try:
        with path.open("r", encoding="utf-8") as handle:
            data = json.load(handle)
    except OSError as exc:
        _fail(f"{path}: {exc}")
    except json.JSONDecodeError as exc:
        _fail(f"{path}: invalid JSON: {exc}")
    if not isinstance(data, dict):
        _fail(f"{path}: top level must be an object")
    return data


def validate(schema: dict[str, Any]) -> None:
    known_top = {"schema_version", "description", "parser_layer", "sections"}
    unknown = set(schema) - known_top
    if unknown:
        _fail(f"unknown top-level key(s): {', '.join(sorted(unknown))}")
    if schema.get("schema_version") != 1:
        _fail("schema_version must be 1")
    if schema.get("parser_layer") != "handwritten":
        _fail("parser_layer must be 'handwritten' (ARCH-13: value assignment "
              "stays in trigger_config.cpp; this schema owns keys/docs/"
              "reload/validation)")
    sections = schema.get("sections")
    if not isinstance(sections, list) or not sections:
        _fail("sections must be a non-empty list")
    seen_sections: set[str] = set()
    for s_index, section in enumerate(sections):
        if not isinstance(section, dict):
            _fail(f"sections[{s_index}] must be an object")
        if set(section) - {"name", "keys"}:
            _fail(f"sections[{s_index}] has unknown key(s): "
                  + ", ".join(sorted(set(section) - {"name", "keys"})))
        name = section.get("name")
        if not isinstance(name, str) or name not in SECTION_ARRAY:
            _fail(f"sections[{s_index}].name: must be one of "
                  + ", ".join(sorted(SECTION_ARRAY)))
        if name in seen_sections:
            _fail(f"duplicate section '{name}'")
        seen_sections.add(name)
        keys = section.get("keys")
        if not isinstance(keys, list) or not keys:
            _fail(f"{name}.keys must be a non-empty list")

        seen: set[str] = set()
        for index, row in enumerate(keys):
            path = f"{name}.keys[{index}]"
            if not isinstance(row, dict):
                _fail(f"{path}: must be an object")
            allowed = {
                "key", "status", "reload", "type", "default", "sample",
                "contract", "note", "reload_note", "error_match",
            }
            extra = set(row) - allowed
            if extra:
                _fail(f"{path}: unknown field(s): {', '.join(sorted(extra))}")
            key = _require_text(row.get("key"), f"{path}.key")
            if key in seen:
                _fail(f"{path}.key: duplicate key '{key}'")
            seen.add(key)

            status = row.get("status")
            if status not in STATUS_CPP:
                _fail(f"{path}.status: must be one of {', '.join(STATUS_CPP)}")
            reload = row.get("reload")
            if reload not in RELOAD_CPP:
                _fail(f"{path}.reload: must be one of {', '.join(RELOAD_CPP)}")
            if status in ("rejected", "future"):
                if reload != "n/a":
                    _fail(f"{path}: rejected/future keys must use reload 'n/a'")
            elif reload == "n/a":
                _fail(f"{path}: implemented/deprecated keys need a reload class")

            for field in TEXT_FIELDS:
                if field not in row:
                    _fail(f"{path}: missing field '{field}'")
                _require_text(row[field], f"{path}.{field}")
            if row["type"] not in TYPES:
                _fail(f"{path}.type: must be one of {', '.join(sorted(TYPES))}")

            if status in ("implemented", "deprecated"):
                if row.get("error_match"):
                    _fail(f"{path}: error_match is only for rejected/future keys")
            else:
                error_match = _require_text(
                    row.get("error_match"), f"{path}.error_match")
                if len(error_match) < 8:
                    _fail(f"{path}.error_match: must be a recognisable error snippet")


# Keys EmitTriggerBase may write under [channel.*]. Keep in lock-step with
# tools/gen/gen_graph/emit_record.cpp. ARCH-13: this set must be a subset of
# the channel.* schema so RecordManifest / trigger.base.ini cannot drift off
# the config fact source.
GEN_GRAPH_CHANNEL_KEYS = frozenset({
    "kind",
    "record_channels",
    "rate_hz",
    "avg_frame_bytes",
})


def check_generated_ini_subset(repo_root: Path, schema: dict[str, Any]) -> None:
    """Fail if gen_graph emits or goldens contain keys outside this schema."""
    channel_keys = {row["key"] for row in _section_rows(schema, "channel.*")}
    missing = sorted(GEN_GRAPH_CHANNEL_KEYS - channel_keys)
    if missing:
        _fail("gen_graph channel key(s) missing from schema: "
              + ", ".join(missing))
    testdata = repo_root / "tools" / "gen" / "gen_graph" / "testdata"
    if not testdata.is_dir():
        return
    keys_by_section = {
        section["name"]: {row["key"] for row in section["keys"]}
        for section in schema["sections"]
    }
    for path in sorted(testdata.rglob("trigger.base.ini")):
        section: str | None = None
        for lineno, raw in enumerate(path.read_text(encoding="utf-8").splitlines(),
                                     start=1):
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            if line.startswith("[") and line.endswith("]"):
                section = line[1:-1]
                continue
            if "=" not in line or section is None:
                _fail(f"{path}:{lineno}: expected key = value under a section")
            key = line.split("=", 1)[0].strip()
            schema_name = "channel.*" if section.startswith("channel.") else section
            allowed = keys_by_section.get(schema_name)
            if allowed is None:
                _fail(f"{path}:{lineno}: section [{section}] is not in the "
                      "trigger config schema")
            if key not in allowed:
                _fail(f"{path}:{lineno}: key '{key}' is not in schema "
                      f"section '{schema_name}'")
            if schema_name == "channel.*" and key not in GEN_GRAPH_CHANNEL_KEYS:
                _fail(f"{path}:{lineno}: key '{key}' is in the schema but "
                      "not in GEN_GRAPH_CHANNEL_KEYS")


def _cpp_string(value: str) -> str:
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def _section_rows(schema: dict[str, Any],
                  name: str | None = None) -> list[dict[str, Any]]:
    if name is None:
        return [dict(row, section=section["name"])
                for section in schema["sections"] for row in section["keys"]]
    for section in schema["sections"]:
        if section["name"] == name:
            return section["keys"]
    _fail(f"unknown section '{name}'")


def _table_cell(text: str) -> str:
    return "`" + text + "`"


def emit_header(schema: dict[str, Any]) -> str:
    out: list[str] = []
    out.append("// GENERATED FILE - DO NOT EDIT.")
    out.append("// Source: tools/gen/config_schema/trigger_config.schema.json")
    out.append("// Regenerate: python3 tools/gen/config_schema/gen_config_schema.py")
    out.append("// Check:      python3 tools/gen/config_schema/gen_config_schema.py --check")
    out.append("//")
    out.append("// ARCH-13: schema-backed INI section metadata. parser_layer is")
    out.append("// handwritten (trigger_config.cpp). test_trigger_config_schema")
    out.append("// cross-checks parser + ReloadRequiresRestart against these tables.")
    out.append("// --check also requires gen_graph trigger.base.ini goldens to emit")
    out.append("// only keys present here (RecordManifest unidirectional subset).")
    out.append("#pragma once")
    out.append("")
    out.append("#include <cstddef>")
    out.append("#include <cstdint>")
    out.append("")
    out.append("namespace thyme {")
    out.append("namespace trigger_cfg {")
    out.append("")
    out.append("enum class ConfigKeyStatus : std::uint8_t {")
    out.append("  kImplemented,")
    out.append("  kDeprecated,")
    out.append("  kRejected,")
    out.append("  kFuture,")
    out.append("};")
    out.append("")
    out.append("enum class ConfigKeyReload : std::uint8_t {")
    out.append("  kHot,              // policy reload: SIGHUP applies it")
    out.append("  kRestartRequired,  // ReloadRequiresRestart rejects the change")
    out.append("  kStartupOnlyKept,  // host-bound; SIGHUP keeps the running value")
    out.append("  kFixedAtStartup,   // only one valid value; no reload delta")
    out.append("  kNotApplicable,    // rejected/future key: never effective")
    out.append("};")
    out.append("")
    out.append("struct ConfigKeySchema {")
    out.append("  const char* section;")
    out.append("  const char* key;")
    out.append("  ConfigKeyStatus status;")
    out.append("  ConfigKeyReload reload;")
    out.append("  const char* type;")
    out.append("  const char* default_value;")
    out.append("  const char* sample;")
    out.append("  const char* error_match;  // rejected/future only; empty otherwise")
    out.append("  const char* contract;")
    out.append("  const char* note;")
    out.append("  const char* reload_note;")
    out.append("};")
    out.append("")
    for section in schema["sections"]:
        name = section["name"]
        array = SECTION_ARRAY[name]
        out.append(f"inline constexpr ConfigKeySchema {array}[] = {{")
        for row in section["keys"]:
            out.append(
                f"    {{\"{name}\", "
                f"{_cpp_string(row['key'])}, {STATUS_CPP[row['status']]}, "
                f"{RELOAD_CPP[row['reload']]}, {_cpp_string(row['type'])}, "
                f"{_cpp_string(row['default'])}, {_cpp_string(row['sample'])}, "
                f"{_cpp_string(row.get('error_match', ''))}, "
                f"{_cpp_string(row['contract'])}, {_cpp_string(row['note'])}, "
                f"{_cpp_string(row['reload_note'])}}},")
        out.append("};")
        out.append(f"inline constexpr std::size_t {array}Count =")
        out.append(f"    sizeof({array}) / sizeof({array}[0]);")
        out.append("")
    out.append("// Union of every schema-backed section (--check-config walks all).")
    out.append("inline constexpr ConfigKeySchema kTriggerConfigSchema[] = {")
    for section in schema["sections"]:
        name = section["name"]
        out.append(f"    // {name}")
        for row in section["keys"]:
            out.append(
                f"    {{\"{name}\", "
                f"{_cpp_string(row['key'])}, {STATUS_CPP[row['status']]}, "
                f"{RELOAD_CPP[row['reload']]}, {_cpp_string(row['type'])}, "
                f"{_cpp_string(row['default'])}, {_cpp_string(row['sample'])}, "
                f"{_cpp_string(row.get('error_match', ''))}, "
                f"{_cpp_string(row['contract'])}, {_cpp_string(row['note'])}, "
                f"{_cpp_string(row['reload_note'])}}},")
    out.append("};")
    out.append("inline constexpr std::size_t kTriggerConfigSchemaCount =")
    out.append("    sizeof(kTriggerConfigSchema) / sizeof(kTriggerConfigSchema[0]);")
    out.append("")
    out.append("}  // namespace trigger_cfg")
    out.append("}  // namespace thyme")
    out.append("")
    return "\n".join(out)


def emit_audit_block(schema: dict[str, Any], name: str) -> str:
    rows = _section_rows(schema, name)
    out: list[str] = []
    out.append("| section | key | status | reload | type | default | contract | note |")
    out.append("| --- | --- | --- | --- | --- | --- | --- | --- |")
    for row in rows:
        out.append(
            f"| `{name}` | "
            f"{_table_cell(row['key'])} | {row['status']} | "
            f"{RELOAD_DOC[row['reload']]} | {_table_cell(row['type'])} | "
            f"{_table_cell(row['default'])} | {row['contract']} | {row['note']} |")
    return "\n".join(out) + "\n"


def emit_reload_block(schema: dict[str, Any]) -> str:
    rows = _section_rows(schema)
    out: list[str] = []
    out.append("| Section / key | SIGHUP |")
    out.append("| --- | --- |")
    for row in rows:
        note = row["reload_note"]
        reload = row["reload"]
        if reload == "hot":
            label = f"hot ({note})"
        elif reload == "restart":
            label = f"**restart required** ({note})"
        elif reload == "startup-kept":
            label = f"startup-only (kept silently: {note})"
        elif reload == "fixed":
            label = f"fixed at startup ({note})"
        else:
            label = "n/a (parse error)"
        out.append(
            f"| `[{row['section']}]` {_table_cell(row['key'])} | {label} |")
    return "\n".join(out) + "\n"


def replace_marked(text: str, begin: str, end: str, body: str, where: str) -> str:
    start = text.find(begin)
    if start < 0:
        _fail(f"{where}: missing marker {begin!r}")
    if text.find(begin, start + 1) >= 0:
        _fail(f"{where}: duplicate marker {begin!r}")
    stop = text.find(end, start + len(begin))
    if stop < 0:
        _fail(f"{where}: missing marker {end!r}")
    return text[: start + len(begin)] + "\n" + body + text[stop:]


def emit_doc(current: str, schema: dict[str, Any]) -> str:
    text = current
    for section in schema["sections"]:
        begin, end = AUDIT_MARKERS[section["name"]]
        text = replace_marked(text, begin, end,
                              emit_audit_block(schema, section["name"]),
                              DOC_REL.as_posix())
    return replace_marked(text, RELOAD_BEGIN, RELOAD_END,
                          emit_reload_block(schema), DOC_REL.as_posix())


def generate(repo_root: Path, schema_path: Path) -> dict[Path, str]:
    schema = load_schema(schema_path)
    validate(schema)
    check_generated_ini_subset(repo_root, schema)
    doc_path = repo_root / DOC_REL
    try:
        doc_text = doc_path.read_text(encoding="utf-8")
    except OSError as exc:
        _fail(f"{doc_path}: {exc}")
    return {
        repo_root / HEADER_REL: emit_header(schema),
        doc_path: emit_doc(doc_text, schema),
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--schema", type=Path, default=DEFAULT_SCHEMA,
                        help=f"schema JSON (default: {DEFAULT_SCHEMA})")
    parser.add_argument("--repo-root", type=Path,
                        default=SCRIPT_DIR.parents[2],
                        help="repository root (default: derived from this script)")
    parser.add_argument("--check", action="store_true",
                        help="verify artifacts are up to date instead of writing")
    args = parser.parse_args(argv)

    repo_root = args.repo_root.resolve()
    try:
        artifacts = generate(repo_root, args.schema.resolve())
    except SchemaError as exc:
        print(f"gen_config_schema: {exc}", file=sys.stderr)
        return EXIT_ERROR

    stale: list[Path] = []
    for path, expected in artifacts.items():
        try:
            current = path.read_text(encoding="utf-8")
        except OSError:
            current = None
        if current == expected:
            continue
        if args.check:
            stale.append(path)
            continue
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(expected, encoding="utf-8")
        print(f"gen_config_schema: wrote {path.relative_to(repo_root)}")
    if stale:
        for path in stale:
            print(f"gen_config_schema: stale generated artifact {path}",
                  file=sys.stderr)
        print("gen_config_schema: rerun without --check to regenerate",
              file=sys.stderr)
        return EXIT_ERROR
    if args.check:
        print("gen_config_schema: generated artifacts are up to date")
    return 0


if __name__ == "__main__":
    sys.exit(main())
