#!/usr/bin/env python3
"""gen_contracts -- emit a thyme contracts header + contracts_include.txt from a
per-project datatypes.json (template reorg phase 02).

Generated structs are **bare, natural-layout payloads**: `types.h` contains
exactly the `datatypes.json` fields — no injected `FrameHeader hdr;` member,
and no `alignas`:
the layout is byte-compatible with naturally-aligned RTE/SDK types, and
cacheline isolation is the container slots' job (ObjectPool / LatestSlot /
ShmSpscRing pad each payload cell to 64 bytes). Framing is the transport
layer's job via `thyme::Frame<Payload>`; the emitted `FillStub` helper is
just `thyme::FrameInit(msg, seq, mono, session)` + `thyme::FrameSeal(msg,
kFrameMagic, kFrameVersion)`. The optional `bytes` key is **payload-only**
size (header excluded) of the natural-layout struct, matching the wire
invariant `FramePodWireBytes<Frame<T>> == sizeof(FrameHeader) + sizeof(T)`.
An array may declare `"length_of": "field"` to name a scalar integer sibling
holding its actual length. Debug builds then overload `FrameSeal` for that
concrete frame type: after the normal seal, an actual length above `count`
(or below zero for a signed length field) clears `hdr.valid`, so the existing
`FrameHeaderOk` boundary rejects it. The overload is excluded by `NDEBUG`;
Release keeps the original generic seal.

Every generated struct carries layout asserts (ABI-03): `sizeof(T) == size`,
`alignof(T) == align`, `offsetof(T, field) == offset` for every field, and for
array fields `sizeof(T::field) == count * stride` plus
`sizeof(T::field[0]) == stride`. The numbers come from `compute_layout()`, a
single natural-alignment model shared with the struct emitter — never
hand-written. A compiler/ABI whose natural alignment disagrees (a packed or
32-bit ABI, structs inheriting different padding rules) fails the build
instead of silently changing the wire layout. Wire endianness is LE-only
(ABI-02, `contracts/abi.h`), so no byte-order asserts are emitted here.

A type may declare `"layout_of": "ns::ReferenceType"` pointing at the
reference (RTE/SDK) type it must stay layout-compatible with. When at least
one type does, an additional optional header `layout_checks.h` is emitted:
it contains `static_assert`s on `sizeof` and every field's `offsetof`
against the reference type, guarded by `#ifdef THYME_LAYOUT_CHECKS`. Include
it only from a TU where the reference type's header is already included
(RTE headers are not necessarily on every include path).

ABI-08 schema validation: ``validate()`` is a collect-all pass that runs before
any emission. It reports every independent violation with its JSON path
(``types[3].fields[2]``), and the CLI exits 2 without writing a single file, so
a bad ``datatypes.json`` can never produce a partial header tree. Besides the
structural rules (unknown keys, duplicate names, unknown types, ``bytes`` vs
natural layout) the pass enforces C++ identifier validity — keywords, names
reserved for the implementation, and names shadowing the generated contract API
— plus explicit ceilings on the dimensions that land in the generated code:
``MAX_TYPES``, ``MAX_FIELDS_PER_STRUCT``, ``MAX_ARRAY_COUNT``,
``MAX_NESTING_DEPTH``, ``MAX_STRUCT_BYTES`` and ``MAX_TOTAL_BYTES``. The
ceilings sit 2-3 orders of magnitude above real contracts and reject unit
errors (a count typed as bytes, a runaway size) instead of emitting a header
that cannot compile.

Python 3 stdlib only. Deterministic output: JSON array order is preserved and no
timestamps or host-specific data are emitted. The one exception is an optional
provenance comment in `types.h` (source version + generation date) controlled by
`--date`; it is comment-only and does not affect the generated code.

Alongside the headers, `template/contract_sizes.txt` records each type's bare
payload size (`<CppType>|<bytes>`, FrameHeader excluded). GEN-08: gen_graph
reads it from the template overlay to reject a contract over the 64 KB Frame
transport limit at generation time (`file:line`) and to emit the framed
wire-size/layout asserts in the generated app.

Usage:
  python3 tools/gen/gen_contracts/gen_contracts.py --in datatypes.json --out <proj>
  python3 tools/gen/gen_contracts/gen_contracts.py --in datatypes.json --out <proj> --check
  python3 tools/gen/gen_contracts/gen_contracts.py --in datatypes.json --out <proj> --graph graph.yml

Exit codes:
  0 success (validation + emit, or --check)
  2 validation/usage/emit error (message to stderr)
"""

from __future__ import annotations

import argparse
import datetime
import json
import re
import sys
from pathlib import Path
from typing import Any

EXIT_ERROR = 2

PRIMITIVES: dict[str, str] = {
    "uint8": "uint8_t",
    "uint16": "uint16_t",
    "uint32": "uint32_t",
    "uint64": "uint64_t",
    "int8": "int8_t",
    "int16": "int16_t",
    "int32": "int32_t",
    "int64": "int64_t",
    "float32": "float",
    "float64": "double",
    "bool": "bool",
}

INTEGER_PRIMITIVES = frozenset({
    "uint8", "uint16", "uint32", "uint64",
    "int8", "int16", "int32", "int64",
})
SIGNED_INTEGER_PRIMITIVES = frozenset({"int8", "int16", "int32", "int64"})

# AUTOSAR (Platform_Types.h) spellings for the same primitives. When --autosar-types
# is passed, the emitted types.h uses these names (uint8, sint8, float32, boolean, …)
# and includes "<namespace>/contracts/Platform_Types.h" so the aliases resolve. The
# canonical datatypes.json names and wire layout are unchanged — only the C++ spelling
# of the field types differs, so contracts_json.h / layout_checks.h are unaffected.
AUTOSAR_PRIMITIVES: dict[str, str] = {
    "uint8": "uint8",
    "uint16": "uint16",
    "uint32": "uint32",
    "uint64": "uint64",
    "int8": "sint8",
    "int16": "sint16",
    "int32": "sint32",
    "int64": "sint64",
    "float32": "float32",
    "float64": "float64",
    "bool": "boolean",
}

PRIMITIVE_SIZES: dict[str, int] = {
    "uint8": 1,
    "uint16": 2,
    "uint32": 4,
    "uint64": 8,
    "int8": 1,
    "int16": 2,
    "int32": 4,
    "int64": 8,
    "float32": 4,
    "float64": 8,
    "bool": 1,
}

# Natural alignment of the same primitives on the little-endian 64-bit ABIs
# thyme targets (x86-64 SysV, AArch64 AAPCS): align == size for every primitive
# in the set, bool included. This table is the single source of the emitted
# layout asserts; a compiler/ABI that disagrees fails the generated
# static_asserts instead of silently changing the wire layout.
PRIMITIVE_ALIGNS: dict[str, int] = {
    "uint8": 1,
    "uint16": 2,
    "uint32": 4,
    "uint64": 8,
    "int8": 1,
    "int16": 2,
    "int32": 4,
    "int64": 8,
    "float32": 4,
    "float64": 8,
    "bool": 1,
}

IDENT_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
# Qualified C++ type reference for "layout_of" (e.g. "rte::EgoMotion").
QUALIFIED_IDENT_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*(::[A-Za-z_][A-Za-z0-9_]*)*$")
NAMESPACE_RE = re.compile(r"^[a-z][a-z0-9_]*$")
# --graph coverage uses a flat per-line regex. Inline comments, quoted scalars,
# and block-style yaml are intentionally not supported.
CPP_TYPE_RE = re.compile(r"^\s*cpp_type:\s*(\S+)")

# frame_magic is a 4-char ASCII stamp; packed big-endian into the frame-header
# magic field (e.g. "TSTH" -> 0x54535448). Must be printable, non-space ASCII.
MAGIC_RE = re.compile(r"^[\x21-\x7e]{4}$")


class _ContractJsonObject(dict):
    duplicate_length_of = False


def _json_object(pairs: list[tuple[str, Any]]) -> _ContractJsonObject:
    obj = _ContractJsonObject()
    for key, value in pairs:
        if key == "length_of" and key in obj:
            obj.duplicate_length_of = True
        obj[key] = value
    return obj


# ---------------------------------------------------------------------------
# ABI-08 schema ceilings.
#
# Every number below lands in generated C++ (array extents, offsetof asserts,
# inlined JSON schemas) or bounds the emit itself, so each gets an explicit
# ceiling. The values are 2-3 orders of magnitude above the largest real
# contract and reject unit errors / absurd inputs before anything is written:
#
#   MAX_TYPES            4096    a project ships tens of contract types; this
#                                bounds emit time and header size.
#   MAX_FIELDS_PER_STRUCT  256    real payloads stay well under ~100 fields;
#                                this bounds the per-struct offsetof asserts.
#   MAX_ARRAY_COUNT    2^25      elements. Covers one 4K RGBA uint8 buffer
#                                (3840*2160*4 = 33,177,600) while rejecting
#                                counts that only make sense as a unit error.
#   MAX_NESTING_DEPTH      16    struct-in-struct chain length. Deeper schemas
#                                are inlined recursively into contracts_json.h
#                                and should be flattened by the contract author.
#   MAX_STRUCT_BYTES    64 MiB   a single payload larger than this belongs in
#                                a pinned_slot channel, not a Frame<T> payload.
#   MAX_TOTAL_BYTES    128 MiB   every type summed; bounds the workspace the
#                                generated contracts imply.
#
# Python integers are unbounded, so these are semantic guards rather than
# overflow guards: they keep count*stride products (and the emitted C++ array
# extents / constexpr asserts) inside a range a compiler and a reader can
# actually handle.
# ---------------------------------------------------------------------------
MAX_TYPES = 4096
MAX_FIELDS_PER_STRUCT = 256
MAX_ARRAY_COUNT = 1 << 25
MAX_NESTING_DEPTH = 16
MAX_STRUCT_BYTES = 64 << 20
MAX_TOTAL_BYTES = 128 << 20

# C++ (through C++20) keywords: a struct/field with one of these names is
# emitted verbatim into the header and can never compile.
CPP_KEYWORDS = frozenset("""
    alignas alignof and and_eq asm auto bitand bitor bool break case catch
    char char8_t char16_t char32_t class compl concept const consteval
    constexpr constinit const_cast continue co_await co_return co_yield decltype
    default delete do double dynamic_cast else enum explicit export extern
    false float for friend goto if inline int long mutable namespace new
    noexcept not not_eq nullptr operator or or_eq private protected public
    register reinterpret_cast requires return short signed sizeof static
    static_assert static_cast struct switch template this thread_local throw
    true try typedef typeid typename union unsigned using virtual void volatile
    wchar_t while xor xor_eq
""".split())

# Names the generated headers already declare at global scope (thyme_types.h /
# types.h / contracts_json.h) plus the primitive spellings and the fixed-width
# typedefs those headers include. A contract type with one of these names either
# conflicts outright or silently shadows a field type alias, so both are
# rejected.
RESERVED_CONTRACT_NAMES = frozenset({
    "Frame",
    "FrameHeader",
    "FramePodWireBytes",
    "FillStub",
    "kFrameMagic",
    "kFrameVersion",
    "AppendInt",
    "AppendUInt",
    "AppendF32",
    "AppendF64",
    "AppendBool",
    "B64",
    "ToJson",
    "size_t",
    "uint8_t",
    "uint16_t",
    "uint32_t",
    "uint64_t",
    "int8_t",
    "int16_t",
    "int32_t",
    "int64_t",
}) | frozenset(PRIMITIVES) | frozenset(AUTOSAR_PRIMITIVES) | frozenset(
    AUTOSAR_PRIMITIVES.values()
)


def magic_u32(s: str) -> int:
    b = s.encode("ascii")
    return (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3]

HEADER_LINES = [
    "// Code generated by gen_contracts. DO NOT EDIT.",
    "#pragma once",
    '#include "thyme/contracts/frame_header.h"',
    '#include "thyme/contracts/frame.h"',
    "#include <cstddef>",
    "#include <cstdint>",
]

FILL_STUB_LINES = [
    "template <typename T>",
    "void FillStub(T& msg, uint32_t seq, uint64_t mono_ns, uint64_t session) {",
    "  thyme::FrameInit(msg, seq, mono_ns, session);",
    "  thyme::FrameSeal(msg, kFrameMagic, kFrameVersion);",
    "}",
]

FILL_STUB_LINES_AUTOSAR = [
    "template <typename T>",
    "void FillStub(T& msg, uint32 seq, uint64 mono_ns, uint64 session) {",
    "  thyme::FrameInit(msg, seq, mono_ns, session);",
    "  thyme::FrameSeal(msg, kFrameMagic, kFrameVersion);",
    "}",
]

# ---------------------------------------------------------------------------
# contracts_json.h emission: per-type JSON Schema + ContractToJson serializers.
#
# Structured types emit field-level JSON (hdr + every field, nested types
# inlined, `count` arrays as JSON arrays). Types with a field named `payload`
# (the FramePodHasPayload blob convention, e.g. image) emit the opaque envelope
# {mono_ns, source_sequence, payload_b64} — same shape the viz bridge has always
# published for opaque channels, now sourced from the struct's own hdr.
# ---------------------------------------------------------------------------

CONTRACTS_JSON_HEADER_LINES = [
    "// Code generated by gen_contracts. DO NOT EDIT.",
    "#pragma once",
    '#include "thyme/contracts/frame_header.h"',
    '#include "types.h"',
    "#include <cstddef>",
    "#include <cstdint>",
    "#include <cstdio>",
    "#include <string>",
]

PRIMITIVE_SCHEMA: dict[str, dict[str, str]] = {
    "uint8": {"type": "integer"},
    "uint16": {"type": "integer"},
    "uint32": {"type": "integer"},
    "uint64": {"type": "integer"},
    "int8": {"type": "integer"},
    "int16": {"type": "integer"},
    "int32": {"type": "integer"},
    "int64": {"type": "integer"},
    "float32": {"type": "number"},
    "float64": {"type": "number"},
    "bool": {"type": "boolean"},
}

PRIMITIVE_APPEND: dict[str, str] = {
    "uint8": "AppendUInt",
    "uint16": "AppendUInt",
    "uint32": "AppendUInt",
    "uint64": "AppendUInt",
    "int8": "AppendInt",
    "int16": "AppendInt",
    "int32": "AppendInt",
    "int64": "AppendInt",
    "float32": "AppendF32",
    "float64": "AppendF64",
    "bool": "AppendBool",
}


def _cpp_string_literal(s: str) -> str:
    out: list[str] = ['"']
    for ch in s:
        if ch == '"':
            out.append('\\"')
        elif ch == "\\":
            out.append("\\\\")
        else:
            out.append(ch)
    out.append('"')
    return "".join(out)


def _opaque_schema() -> dict[str, Any]:
    # The opaque envelope the serializer emits for `payload` blob types.
    return {
        "type": "object",
        "properties": {
            "mono_ns": {"type": "integer"},
            "source_sequence": {"type": "integer"},
            "payload_b64": {"type": "string"},
        },
    }


def _field_schema(by_name: dict[str, dict[str, Any]], ftype: str,
                  count: int | None) -> dict[str, Any]:
    if count is not None and count > 1:
        return {"type": "array", "items": _field_schema(by_name, ftype, None)}
    if ftype in PRIMITIVE_SCHEMA:
        return dict(PRIMITIVE_SCHEMA[ftype])
    return _type_schema(by_name, by_name[ftype])


def _type_schema(by_name: dict[str, dict[str, Any]],
                 t: dict[str, Any]) -> dict[str, Any]:
    props: dict[str, Any] = {}
    for f in t["fields"]:
        props[f["name"]] = _field_schema(by_name, f["type"], f.get("count"))
    return {"type": "object", "properties": props}


def _is_opaque(t: dict[str, Any]) -> bool:
    return any(f["name"] == "payload" for f in t["fields"])


def _emit_value_serialize(fname: str, ftype: str, count: int | None,
                          indent: str, prefix: str) -> list[str]:
    lines: list[str] = []
    if count is not None and count > 1:
        lines.append(f"{indent}out += '[';")
        lines.append(f"{indent}for (std::size_t i = 0; i < {count}; ++i) {{")
        lines.append(f"{indent}  if (i) out += ',';")
        if ftype in PRIMITIVE_APPEND:
            lines.append(f"{indent}  {PRIMITIVE_APPEND[ftype]}(out, v.{prefix}{fname}[i]);")
        else:
            lines.append(f"{indent}  ToJson(v.{prefix}{fname}[i], out);")
        lines.append(f"{indent}}}")
        lines.append(f"{indent}out += ']';")
    else:
        if ftype in PRIMITIVE_APPEND:
            lines.append(f"{indent}{PRIMITIVE_APPEND[ftype]}(out, v.{prefix}{fname});")
        else:
            lines.append(f"{indent}ToJson(v.{prefix}{fname}, out);")
    return lines


def _emit_serializer_body(t: dict[str, Any], prefix: str, indent: str) -> list[str]:
    lines: list[str] = []
    lines.append(f"{indent}out += '{{';")
    for i, f in enumerate(t["fields"]):
        if i > 0:
            lines.append(f'{indent}out += ",\\"{f["name"]}\\":";')
        else:
            lines.append(f'{indent}out += "\\"{f["name"]}\\":";')
        lines.extend(_emit_value_serialize(f["name"], f["type"],
                                           f.get("count"), indent, prefix))
    lines.append(f"{indent}out += '}}';")
    return lines


def _emit_serializer(t: dict[str, Any]) -> str:
    name = t["name"]
    lines: list[str] = []
    if _is_opaque(t):
        # Opaque blob: publish the envelope, sourcing timing from the struct's
        # own header and base64-ing the payload member (F6) — padding never
        # reaches the wire.
        lines.append(f"inline void ToJson(const thyme::Frame<{name}>& v, std::string& out) {{")
        lines.append('  out += "{\\"mono_ns\\":";')
        lines.append("  AppendUInt(out, v.hdr.mono_ns);")
        lines.append('  out += ",\\"source_sequence\\":";')
        lines.append("  AppendUInt(out, v.hdr.sequence);")
        lines.append('  out += ",\\"payload_b64\\":\\"";')
        lines.append("  B64(out, reinterpret_cast<const uint8_t*>(&v.payload), sizeof(v.payload));")
        lines.append('  out += "\\"";')
        lines.append("  out += '}';")
        lines.append("}")
    else:
        # Structured payload: the transport-layer serializer reads the bare
        # payload member directly (no embedded header — F2).
        lines.append(f"inline void ToJson(const thyme::Frame<{name}>& v, std::string& out) {{")
        lines.extend(_emit_serializer_body(t, "payload.", "  "))
        lines.append("}")
        # Bare overload: nested structured members recurse here so a nested
        # bare payload serializes without an inner header.
        lines.append("")
        lines.append(f"inline void ToJson(const {name}& v, std::string& out) {{")
        lines.extend(_emit_serializer_body(t, "", "  "))
        lines.append("}")
    return "\n".join(lines)


HELPERS_LINES = [
    "inline void AppendInt(std::string& out, int64_t v) { out += std::to_string(v); }",
    "inline void AppendUInt(std::string& out, uint64_t v) { out += std::to_string(v); }",
    "inline void AppendF32(std::string& out, float v) {",
    '  char b[32];',
    '  std::snprintf(b, sizeof(b), "%.9g", static_cast<double>(v));',
    "  out += b;",
    "}",
    "inline void AppendF64(std::string& out, double v) {",
    '  char b[32];',
    '  std::snprintf(b, sizeof(b), "%.17g", v);',
    "  out += b;",
    "}",
    "inline void AppendBool(std::string& out, bool v) { out += v ? \"true\" : \"false\"; }",
    "inline void B64(std::string& out, const uint8_t* p, std::size_t n) {",
    '  static const char kTab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";',
    "  for (std::size_t i = 0; i < n; i += 3) {",
    "    const uint32_t a = p[i];",
    "    const uint32_t b = (i + 1 < n) ? p[i + 1] : 0;",
    "    const uint32_t c = (i + 2 < n) ? p[i + 2] : 0;",
    "    const uint32_t x = (a << 16) | (b << 8) | c;",
    "    out += kTab[(x >> 18) & 63];",
    "    out += kTab[(x >> 12) & 63];",
    "    out += (i + 1 < n) ? kTab[(x >> 6) & 63] : '=';",
    "    out += (i + 2 < n) ? kTab[x & 63] : '=';",
    "  }",
    "}",
    "",
]


def emit_contracts_json(doc: dict[str, Any]) -> str:
    by_name: dict[str, dict[str, Any]] = {}
    order: list[str] = []
    for t in doc.get("types", []):
        if isinstance(t, dict) and isinstance(t.get("name"), str):
            by_name[t["name"]] = t
            order.append(t["name"])

    lines = list(CONTRACTS_JSON_HEADER_LINES)
    lines += [""]
    lines.append("namespace thyme_contracts_json {")
    lines.append("")
    lines += [l for l in HELPERS_LINES]
    lines.append("")

    for name in order:
        t = by_name[name]
        schema = _opaque_schema() if _is_opaque(t) else _type_schema(by_name, t)
        schema_lit = _cpp_string_literal(json.dumps(schema, separators=(",", ":")))
        lines.append(f"inline const char* kSchema_{name} = {schema_lit};")
    lines.append("")

    for name in order:
        lines.append(_emit_serializer(by_name[name]))
        lines.append("")

    lines.append("}  // namespace thyme_contracts_json")
    lines.append("")
    return "\n".join(lines) + "\n"


def error(msg: str) -> None:
    print(f"gen_contracts: error: {msg}", file=sys.stderr)


def warn(msg: str) -> None:
    print(f"gen_contracts: warning: {msg}", file=sys.stderr)


def _align_up(value: int, align: int) -> int:
    return (value + align - 1) // align * align


def compute_layout(doc: dict[str, Any]) -> dict[str, dict[str, Any]]:
    """Compute the natural-alignment layout of every emitted struct.

    This mirrors the C++ rules that lay out the generated decls: each field
    starts at the next multiple of its type's alignment, an array occupies
    ``count * sizeof(element)`` with the element's alignment (so the stride is
    exactly ``sizeof(element)``, padding included), and the struct size is
    rounded up to its strictest member alignment. An empty struct is 1/1
    (C++ requires every complete object to occupy at least one byte).

    Returns ``{type_name: {size, align, fields: [{name, offset, count, stride,
    bytes}]}}`` in declaration order. Types referencing an undefined name are
    left out — ``validate()`` reports those separately. Malformed entries
    (non-list ``fields``, non-hashable ``type``) are skipped the same way: the
    caller may probe the model before structural validation has rejected the
    document, and it must never crash on it.
    """
    layouts: dict[str, dict[str, Any]] = {}
    types = doc.get("types", [])
    if not isinstance(types, list):
        return layouts
    for t in types:
        if not isinstance(t, dict):
            continue
        name = t.get("name")
        if not isinstance(name, str):
            continue
        raw_fields = t.get("fields")
        if not isinstance(raw_fields, list):
            continue
        offset = 0
        align = 1
        fields: list[dict[str, Any]] = []
        for f in raw_fields:
            if not isinstance(f, dict):
                continue
            ftype = f.get("type")
            count = f.get("count", 1)
            if isinstance(count, bool) or not isinstance(count, int) or count < 1:
                count = 1
            if isinstance(ftype, str) and ftype in PRIMITIVE_SIZES:
                elem_size = PRIMITIVE_SIZES[ftype]
                elem_align = PRIMITIVE_ALIGNS[ftype]
            elif isinstance(ftype, str) and ftype in layouts:
                elem_size = layouts[ftype]["size"]
                elem_align = layouts[ftype]["align"]
            else:
                continue
            offset = _align_up(offset, elem_align)
            fields.append({
                "name": f.get("name"),
                "type": ftype,
                "offset": offset,
                "count": count,
                "stride": elem_size,
                "bytes": elem_size * count,
            })
            offset += elem_size * count
            align = max(align, elem_align)
        layouts[name] = {
            "size": max(_align_up(offset, align), 1),
            "align": align,
            "fields": fields,
        }
    return layouts


def validate(doc: Any) -> tuple[list[str], list[str]]:
    errors: list[str] = []
    warnings: list[str] = []

    if not isinstance(doc, dict):
        return ["top-level JSON must be an object"], []

    allowed_top = {"version", "namespace", "types", "frame_magic", "frame_version",
                   "source_version"}
    for key in doc:
        if key not in allowed_top:
            errors.append(f"unknown top-level key: {key!r}")

    if "version" not in doc:
        errors.append("missing required key: version")
    elif doc["version"] != 1:
        errors.append(f"version must be 1, got {doc['version']!r}")

    if "frame_magic" not in doc:
        errors.append("missing required key: frame_magic")
    elif not isinstance(doc["frame_magic"], str) or not MAGIC_RE.match(doc["frame_magic"]):
        errors.append(
            f"frame_magic must be 4 printable ASCII chars, got {doc['frame_magic']!r}"
        )

    if "frame_version" not in doc:
        errors.append("missing required key: frame_version")
    else:
        fv = doc["frame_version"]
        if isinstance(fv, bool) or not isinstance(fv, int) or not (1 <= fv <= 0xFFFF):
            errors.append(f"frame_version must be an integer in [1, 65535], got {fv!r}")

    if "namespace" not in doc:
        errors.append("missing required key: namespace")
    elif not isinstance(doc["namespace"], str) or not NAMESPACE_RE.match(doc["namespace"]):
        errors.append(f"namespace must match [a-z][a-z0-9_]*, got {doc['namespace']!r}")

    if "types" not in doc:
        errors.append("missing required key: types")
    elif not isinstance(doc["types"], list) or not doc["types"]:
        errors.append("types must be a non-empty array")
    else:
        if len(doc["types"]) > MAX_TYPES:
            errors.append(
                f"types: {len(doc['types'])} entries exceeds the maximum "
                f"{MAX_TYPES}"
            )
        names_defined: set[str] = set()
        for i, t in enumerate(doc["types"]):
            _validate_type(t, i, names_defined, errors, warnings)

        # `bytes` is the declared wire size. ABI-03 makes the natural layout
        # model authoritative: if the two disagree the emitted header could
        # never compile, so fail here with a precise message instead.
        layouts = None
        for t in doc["types"]:
            if not isinstance(t, dict) or "bytes" not in t:
                continue
            b = t["bytes"]
            n = t.get("name")
            if (
                isinstance(n, str)
                and not isinstance(b, bool)
                and isinstance(b, int)
                and b >= 1
            ):
                if layouts is None:
                    layouts = compute_layout(doc)
                if n in layouts and b != layouts[n]["size"]:
                    errors.append(
                        f"type {n!r}: bytes {b} does not match the computed "
                        f"natural layout size {layouts[n]['size']}"
                    )

        # Size/depth ceilings are derived from the layout model, so only check
        # them once the document is structurally sound: an unknown field type
        # is dropped by compute_layout() and would skew the reported size.
        if not errors:
            _validate_layout_limits(doc, errors)

    return errors, warnings


def _reserved_impl_ident(name: str) -> bool:
    """Identifiers containing ``__`` or starting with ``_`` + uppercase are
    reserved to the C++ implementation in every scope."""
    return "__" in name or (len(name) > 1 and name[0] == "_" and name[1].isupper())


def _name_rule_errors(path: str, name: str, *, is_type: bool) -> list[str]:
    """C++ language rules a name must satisfy to survive emission verbatim.

    Names are never sanitized: they land directly in ``struct <name>``,
    ``T::<field>`` and the contracts_json API. A keyword, an implementation-
    reserved spelling or a name shadowing generated glue makes the header
    uncompilable, so it is a schema error rather than a later build error."""
    errs: list[str] = []
    if name in CPP_KEYWORDS:
        errs.append(f"{path}: name {name!r} is a C++ keyword")
        return errs
    if is_type and name[0] == "_":
        errs.append(
            f"{path}: name {name!r} is reserved for the C++ implementation "
            f"(a global name may not start with '_')"
        )
    elif _reserved_impl_ident(name):
        errs.append(
            f"{path}: name {name!r} is reserved for the C++ implementation "
            f"('__' or '_' + uppercase)"
        )
    if is_type and name in RESERVED_CONTRACT_NAMES:
        errs.append(
            f"{path}: name {name!r} collides with a generated contract identifier"
        )
    return errs


def _validate_layout_limits(doc: dict[str, Any], errors: list[str]) -> None:
    """ABI-08 struct-size, total-size and nesting-depth ceilings.

    Runs after structural validation so every referenced type resolves.
    Sizes come from the same ``compute_layout()`` model the emitter uses, so
    the ceiling applies to exactly the C++ that would have been generated."""
    layouts = compute_layout(doc)
    depths: dict[str, int] = {}
    total = 0
    for i, t in enumerate(doc["types"]):
        name = t["name"]
        size = layouts[name]["size"]
        total += size
        if size > MAX_STRUCT_BYTES:
            errors.append(
                f"types[{i}]: natural layout size {size} exceeds the maximum "
                f"struct size {MAX_STRUCT_BYTES}"
            )

        # Depth is the longest struct-in-struct chain ending at this type:
        # a leaf struct is depth 1, +1 for every struct-typed field.
        best = 1
        best_field = None
        best_ref = None
        for j, f in enumerate(t["fields"]):
            ftype = f["type"]
            if ftype in PRIMITIVES or ftype not in depths:
                continue
            cand = depths[ftype] + 1
            if cand > best:
                best = cand
                best_field = j
                best_ref = ftype
        depths[name] = best
        if best > MAX_NESTING_DEPTH:
            errors.append(
                f"types[{i}].fields[{best_field}]: nesting depth {best} exceeds "
                f"the maximum {MAX_NESTING_DEPTH} (deepest chain through "
                f"{best_ref!r})"
            )
    if total > MAX_TOTAL_BYTES:
        errors.append(
            f"types: total natural layout size {total} exceeds the maximum "
            f"{MAX_TOTAL_BYTES}"
        )


def _validate_type(
    t: Any,
    i: int,
    names_defined: set[str],
    errors: list[str],
    warnings: list[str],
) -> None:
    pfx = f"types[{i}]"
    if not isinstance(t, dict):
        errors.append(f"{pfx}: must be an object")
        return

    for key in t:
        if key not in {"name", "fields", "bytes", "layout_of"}:
            errors.append(f"{pfx}: unknown key: {key!r}")

    if "layout_of" in t:
        lo = t["layout_of"]
        if not isinstance(lo, str) or not QUALIFIED_IDENT_RE.match(lo):
            errors.append(f"{pfx}: bad qualified type reference layout_of: {lo!r}")

    name = t.get("name")
    name_ok = isinstance(name, str) and bool(IDENT_RE.match(name))
    if "name" not in t:
        errors.append(f"{pfx}: missing required key: name")
    elif not name_ok:
        errors.append(f"{pfx}: bad identifier name: {name!r}")
    elif name in names_defined:
        errors.append(f"{pfx}: duplicate type name: {name!r}")
    if name_ok:
        errors.extend(_name_rule_errors(pfx, name, is_type=True))

    if "bytes" in t:
        b = t["bytes"]
        if isinstance(b, bool) or not isinstance(b, int) or b < 1:
            errors.append(f"{pfx}: bytes must be a positive integer, got {b!r}")
        elif b > MAX_STRUCT_BYTES:
            errors.append(
                f"{pfx}: bytes {b} exceeds the maximum struct size "
                f"{MAX_STRUCT_BYTES}"
            )

    if "fields" not in t:
        errors.append(f"{pfx}: missing required key: fields")
        return
    fields = t["fields"]
    if not isinstance(fields, list) or not fields:
        errors.append(f"{pfx}: fields must be a non-empty array")
        return
    if len(fields) > MAX_FIELDS_PER_STRUCT:
        errors.append(
            f"{pfx}: {len(fields)} fields exceeds the maximum "
            f"{MAX_FIELDS_PER_STRUCT}"
        )

    seen_fields: set[str] = set()
    for j, f in enumerate(fields):
        fp = f"{pfx}.fields[{j}]"
        if not isinstance(f, dict):
            errors.append(f"{fp}: must be an object")
            continue
        for key in f:
            if key not in {"name", "type", "count", "length_of"}:
                errors.append(f"{fp}: unknown key: {key!r}")

        fname = f.get("name")
        if "name" not in f:
            errors.append(f"{fp}: missing required key: name")
        elif not isinstance(fname, str) or not IDENT_RE.match(fname):
            errors.append(f"{fp}: bad identifier name: {fname!r}")
        else:
            if fname in seen_fields:
                errors.append(f"{fp}: duplicate field name: {fname!r}")
            seen_fields.add(fname)
            errors.extend(_name_rule_errors(fp, fname, is_type=False))
            # `struct S { uint8_t S; };` is ill-formed: a member may not reuse
            # its class name.
            if isinstance(name, str) and fname == name:
                errors.append(
                    f"{fp}: field name {fname!r} collides with its struct name"
                )

        ftype = f.get("type")
        if "type" not in f:
            errors.append(f"{fp}: missing required key: type")
        elif not isinstance(ftype, str) or not IDENT_RE.match(ftype):
            errors.append(f"{fp}: bad identifier type: {ftype!r}")
        elif ftype not in PRIMITIVES and ftype not in names_defined:
            errors.append(
                f"{fp}: type {ftype!r} is not a primitive or a previously-defined type"
            )

        if "count" in f:
            c = f["count"]
            if isinstance(c, bool) or not isinstance(c, int) or c < 1:
                errors.append(f"{fp}: count must be an integer >= 1, got {c!r}")
            elif c > MAX_ARRAY_COUNT:
                errors.append(
                    f"{fp}: count {c} exceeds the maximum array count "
                    f"{MAX_ARRAY_COUNT}"
                )

        if ftype == "bool" and isinstance(fname, str):
            warnings.append(f"{fp}: field {fname!r} is bool (wire-ABI smell; prefer uint8)")

    fields_by_name = {
        f["name"]: f for f in fields
        if isinstance(f, dict) and isinstance(f.get("name"), str)
    }
    for j, f in enumerate(fields):
        if not isinstance(f, dict) or "length_of" not in f:
            continue
        fp = f"{pfx}.fields[{j}]"
        if getattr(f, "duplicate_length_of", False):
            errors.append(f"{fp}: duplicate key: 'length_of'")
        length_of = f["length_of"]
        if not isinstance(length_of, str) or not IDENT_RE.match(length_of):
            errors.append(f"{fp}: length_of must be a field identifier, got {length_of!r}")
            continue
        count = f.get("count")
        if isinstance(count, bool) or not isinstance(count, int) or count <= 1:
            errors.append(f"{fp}: length_of requires an array field with count > 1")
        if length_of == f.get("name"):
            errors.append(f"{fp}: length_of must not reference the array field itself")
            continue
        target = fields_by_name.get(length_of)
        if target is None:
            errors.append(f"{fp}: length_of references unknown field {length_of!r}")
            continue
        target_count = target.get("count", 1)
        target_type = target.get("type")
        if (target_count != 1 or not isinstance(target_type, str)
                or target_type not in INTEGER_PRIMITIVES):
            errors.append(f"{fp}: length_of must reference a scalar integer field")

    # A field named `payload` switches the whole struct to the opaque byte
    # envelope (contracts_json.h emits {mono_ns, source_sequence, payload_b64}
    # and base64s only that member). Any sibling field would be silently
    # dropped from the published JSON, so the schema must say what it means.
    for j, f in enumerate(fields):
        if isinstance(f, dict) and f.get("name") == "payload" and len(fields) > 1:
            errors.append(
                f"{pfx}.fields[{j}]: field 'payload' makes the struct an opaque "
                f"byte envelope; it must be the only field"
            )
            break

    if name_ok:
        names_defined.add(name)


def _emit_layout_asserts(name: str, info: dict[str, Any],
                         wire_bytes: int | None) -> list[str]:
    """ABI-03: sizeof/offsetof/alignof asserts for one struct, straight from
    compute_layout(). Only `static_assert`, so no code or warnings are emitted
    under -Wall -Wextra -Wconversion -Werror. `T::field` / `T::field[0]` are
    unevaluated sizeof operands (valid C++17 for non-static data members, no
    null dereference)."""
    if wire_bytes is not None:
        size_msg = f"gen_contracts: {name} wire size drift"
    else:
        size_msg = f"gen_contracts: {name} size drift"
    lines = [
        f'static_assert(sizeof({name}) == {info["size"]}, "{size_msg}");',
        f'static_assert(alignof({name}) == {info["align"]}, '
        f'"gen_contracts: {name} alignment drift");',
    ]
    for f in info["fields"]:
        fname = f["name"]
        lines.append(
            f'static_assert(offsetof({name}, {fname}) == {f["offset"]}, '
            f'"gen_contracts: {name}.{fname} offset drift");'
        )
        if f["count"] > 1:
            lines.append(
                f'static_assert(sizeof({name}::{fname}) == {f["bytes"]}, '
                f'"gen_contracts: {name}.{fname} array size drift");'
            )
            lines.append(
                f'static_assert(sizeof({name}::{fname}[0]) == {f["stride"]}, '
                f'"gen_contracts: {name}.{fname} array stride drift");'
            )
    return lines


def _emit_structs(doc: dict[str, Any], prim_map: dict[str, str]) -> list[str]:
    """Emit the bare struct definitions plus their ABI-03 layout asserts (the
    only part shared across the internal types.h and the shareable
    thyme_types.h). Returns a list of lines."""
    layouts = compute_layout(doc)
    lines: list[str] = []
    for t in doc["types"]:
        name = t["name"]
        lines.append("")
        lines.append(f"struct {name} {{")
        for f in t["fields"]:
            ctype = prim_map.get(f["type"], f["type"])
            count = f.get("count")
            if count is not None and count > 1:
                lines.append(f"  {ctype} {f['name']}[{count}];")
            else:
                lines.append(f"  {ctype} {f['name']};")
        lines.append("};")
        bytes_ = t.get("bytes")
        if not isinstance(bytes_, int) or isinstance(bytes_, bool):
            bytes_ = None
        lines.extend(_emit_layout_asserts(name, layouts[name], bytes_))
    return lines


def _provenance_lines(doc: dict[str, Any], gen_date: str | None) -> list[str]:
    """Provenance comment lines (source version + generation date), or []."""
    out: list[str] = []
    src_ver = doc.get("source_version")
    if isinstance(src_ver, str) and src_ver:
        out.append(f"// Source version: {src_ver}")
    if gen_date:
        out.append(f"// Generated: {gen_date}")
    return out


def _emit_length_of_checks(doc: dict[str, Any]) -> list[str]:
    checked = [
        t for t in doc["types"]
        if any("length_of" in f for f in t["fields"])
    ]
    if not checked:
        return []
    lines = ["", "#ifndef NDEBUG", "namespace thyme {"]
    for t in checked:
        name = t["name"]
        fields_by_name = {f["name"]: f for f in t["fields"]}
        lines += [
            "",
            f"inline void FrameSeal(Frame<{name}>& msg, uint32_t magic, uint16_t version) {{",
            f"  FrameSeal<Frame<{name}>>(msg, magic, version);",
        ]
        for f in t["fields"]:
            if "length_of" in f:
                length_name = f["length_of"]
                condition = f"msg.payload.{length_name} > {f['count']}"
                if fields_by_name[length_name]["type"] in SIGNED_INTEGER_PRIMITIVES:
                    condition = f"msg.payload.{length_name} < 0 || {condition}"
                lines += [
                    f"  if ({condition}) {{",
                    "    msg.hdr.valid = 0;",
                    "  }",
                ]
        lines.append("}")
    lines += ["", "}  // namespace thyme", "#endif  // NDEBUG"]
    return lines


def emit_header(doc: dict[str, Any], with_fill_stub: bool, autosar: bool = False,
                gen_date: str | None = None) -> str:
    """Emit the internal runtime header (types.h). The bare struct definitions
    live in the shareable thyme_types.h and are pulled in here with a single
    include, so the two files can never drift (thyme_types.h is the sole holder)."""

    lines = list(HEADER_LINES)
    # Provenance comment (source version + generation date) sits right below the
    # "DO NOT EDIT" banner — comment-only, no effect on the generated code.
    provenance = _provenance_lines(doc, gen_date)
    if provenance:
        lines[1:1] = provenance
    if autosar:
        # Swap the <cstdint> include for the namespace Platform_Types.h so the
        # AUTOSAR aliases (uint8/sint8/float32/boolean) resolve. <cstddef> stays
        # for size_t; frame_header.h pulls <cstdint> transitively.
        ns = doc["namespace"]
        lines = [l for l in lines if l != "#include <cstdint>"]
        lines.append(f'#include "{ns}/contracts/Platform_Types.h"')
    if with_fill_stub:
        lines.append("#include <cstring>")
    lines += ['#include "thyme_types.h"']
    lines += ["", "using thyme::FrameHeader;", "using thyme::FramePodWireBytes;"]
    if autosar:
        lines += [
            "",
            f"inline constexpr uint32 kFrameMagic = 0x{magic_u32(doc['frame_magic']):08X}u;",
            f"inline constexpr uint16 kFrameVersion = {doc['frame_version']};",
        ]
    else:
        lines += [
            "",
            f"inline constexpr uint32_t kFrameMagic = 0x{magic_u32(doc['frame_magic']):08X}u;",
            f"inline constexpr uint16_t kFrameVersion = {doc['frame_version']};",
        ]

    lines.extend(_emit_length_of_checks(doc))
    if with_fill_stub:
        lines.append("")
        lines.extend(FILL_STUB_LINES_AUTOSAR if autosar else FILL_STUB_LINES)

    return "\n".join(lines) + "\n"


def emit_thyme_types(doc: dict[str, Any], autosar: bool = False,
                     gen_date: str | None = None) -> str:
    """Emit the self-contained shareable header (thyme_types.h) for handing to
    algorithm teams: only the bare struct definitions and their generated
    layout asserts, plus a primitive provider include (Platform_Types.h in
    AUTOSAR mode, <cstdint> otherwise). No thyme runtime includes, no FillStub,
    no frame constants. This is the sole holder of the struct definitions;
    types.h includes it."""
    prim_map = AUTOSAR_PRIMITIVES if autosar else PRIMITIVES
    lines = [
        "// Code generated by gen_contracts. DO NOT EDIT.",
    ]
    provenance = _provenance_lines(doc, gen_date)
    lines += provenance
    if autosar:
        lines += ["#pragma once", "#include <cstddef>", '#include "Platform_Types.h"']
    else:
        lines += ["#pragma once", "#include <cstddef>", "#include <cstdint>"]
    lines += [
        "",
        "// ABI-03: each struct is followed by generated sizeof/offsetof/alignof",
        "// asserts (arrays: count * stride and element stride) computed by the",
        "// generator's natural-alignment model. A failure means the layout really",
        "// changed: bump the contract/frame version, never edit the assert.",
    ]
    lines += _emit_structs(doc, prim_map)
    return "\n".join(lines) + "\n"


def emit_contract_sizes(doc: dict[str, Any]) -> str:
    """GEN-08: bare payload struct sizes (FrameHeader excluded), consumed by
    gen_graph's template catalog. The generator uses them for the generation-time
    64 KB Frame transport check and for the generated framed-wire-size assert;
    the authoritative bare sizeof assert stays in types.h (ABI-03)."""
    layouts = compute_layout(doc)
    lines = [
        "# Code generated by gen_contracts. DO NOT EDIT.",
        "# Bare payload struct sizes, FrameHeader excluded. Consumed by gen_graph",
        "# (GEN-08) for the 64 KB Frame transport limit. Format: <CppType>|<bytes>",
    ]
    for t in doc["types"]:
        name = t.get("name")
        if isinstance(name, str):
            lines.append(f"{name}|{layouts[name]['size']}")
    return "\n".join(lines) + "\n"


def emit_contract_layout(doc: dict[str, Any]) -> str:
    """ABI-09: runtime field-layout table for the record schema sidecar.

    The numbers come from the same ``compute_layout()`` model that emits the
    ABI-03 asserts, and every table entry is static_assert-bound to the
    compiler's real ``offsetof``/``sizeof``. A recorder can therefore publish
    a field-level ``schema.json`` without a diagnostics build, and a drift
    between the model and the compiler fails this header's build instead of
    writing a wrong sidecar.

    Emitted for every project (it is part of the contracts output set), so a
    host can wire ``thyme_contract_layout::FindContractLayout`` into
    ``GetThymeRecordTable().manifest`` unconditionally."""
    layouts = compute_layout(doc)
    lines = [
        "// Code generated by gen_contracts. DO NOT EDIT.",
        "#pragma once",
        "#include <cstddef>",
        "#include <cstring>",
        '#include "types.h"',
        '#include "thyme/record/record_schema.h"',
        "",
        "// ABI-09: runtime layout table for the package schema sidecar. The",
        "// values mirror compute_layout() and are static_assert-bound below to",
        "// the compiler's offsetof/sizeof, so a model/compiler disagreement",
        "// fails this build instead of publishing a wrong schema.",
        "namespace thyme_contract_layout {",
        "",
    ]
    for t in doc["types"]:
        name = t["name"]
        info = layouts[name]
        lines.append(
            f"inline constexpr thyme::schema::FieldLayout k{name}Fields[] = {{"
        )
        for f in info["fields"]:
            lines.append(
                f'    {{"{f["name"]}", "{f["type"]}", {f["offset"]}u, '
                f'{f["bytes"]}u, {f["count"]}u, {f["stride"]}u}},'
            )
        lines.append("};")
        for i, f in enumerate(info["fields"]):
            fname = f["name"]
            lines.append(
                f"static_assert(k{name}Fields[{i}].offset == "
                f"offsetof({name}, {fname}), "
                f'"gen_contracts: {name}.{fname} schema offset drift");'
            )
            lines.append(
                f"static_assert(k{name}Fields[{i}].size == sizeof({name}::{fname}), "
                f'"gen_contracts: {name}.{fname} schema size drift");'
            )
            if f["count"] > 1:
                lines.append(
                    f"static_assert(k{name}Fields[{i}].stride == "
                    f"sizeof({name}::{fname}[0]), "
                    f'"gen_contracts: {name}.{fname} schema stride drift");'
                )
        lines.append("")
    lines.append("inline constexpr thyme::schema::ContractLayout kContracts[] = {")
    for t in doc["types"]:
        name = t["name"]
        info = layouts[name]
        count = len(info["fields"])
        lines.append(
            f'    {{"{name}", {info["size"]}u, {count}u, '
            f'{info["align"]}u, k{name}Fields}},'
        )
    lines.append("};")
    lines.append("")
    lines.append("inline constexpr std::size_t kContractCount =")
    lines.append("    sizeof(kContracts) / sizeof(kContracts[0]);")
    for i, t in enumerate(doc["types"]):
        name = t["name"]
        lines.append(
            f"static_assert(kContracts[{i}].payload_bytes == sizeof({name}), "
            f'"gen_contracts: {name} schema payload size drift");'
        )
        lines.append(
            f"static_assert(kContracts[{i}].align == alignof({name}), "
            f'"gen_contracts: {name} schema align drift");'
        )
    lines += [
        "",
        "// ABI-09 contract lookup used by the recorder's schema writer. Returns",
        "// nullptr for an unknown type name (a stale generated table, or a",
        "// channel recorded by another build) so the sidecar can say",
        '// "layout unavailable" instead of guessing.',
        "inline const thyme::schema::ContractLayout* FindContractLayout(",
        "    const char* cpp_type) {",
        "  if (cpp_type == nullptr) return nullptr;",
        "  for (std::size_t i = 0; i < kContractCount; ++i) {",
        "    if (std::strcmp(kContracts[i].cpp_type, cpp_type) == 0) {",
        "      return &kContracts[i];",
        "    }",
        "  }",
        "  return nullptr;",
        "}",
        "",
        "}  // namespace thyme_contract_layout",
        "",
    ]
    return "\n".join(lines)


def emit_layout_checks(doc: dict[str, Any]) -> str | None:
    """Optional header with layout-equivalence static_asserts for every type
    carrying a "layout_of" reference. Returns None when no type uses it."""
    checked = [t for t in doc["types"] if "layout_of" in t]
    if not checked:
        return None
    lines = [
        "// Code generated by gen_contracts. DO NOT EDIT.",
        "#pragma once",
        "// Optional layout-equivalence checks against the reference (RTE/SDK)",
        "// types named by \"layout_of\" in datatypes.json. This header only",
        "// references those external types; include it from a TU that already",
        "// includes the reference headers, and define THYME_LAYOUT_CHECKS to",
        "// enable the assertions (compile-time only, no emitted code).",
        '#include "types.h"',
        "#include <cstddef>",
        "",
        "#ifdef THYME_LAYOUT_CHECKS",
    ]
    for t in checked:
        name = t["name"]
        ref = t["layout_of"]
        lines.append("")
        lines.append(
            f'static_assert(sizeof({name}) == sizeof({ref}), '
            f'"gen_contracts: {name} layout differs from {ref}");'
        )
        for f in t["fields"]:
            lines.append(
                f'static_assert(offsetof({name}, {f["name"]}) == '
                f'offsetof({ref}, {f["name"]}), '
                f'"gen_contracts: {name}.{f["name"]} offset differs from {ref}");'
            )
    lines += ["", "#endif  // THYME_LAYOUT_CHECKS"]
    return "\n".join(lines) + "\n"


def plan_outputs(doc: dict[str, Any], with_fill_stub: bool, autosar: bool = False,
                 gen_date: str | None = None) -> list[tuple[str, str]]:
    ns = doc["namespace"]
    outputs = [
        (f"include/{ns}/contracts/types.h",
         emit_header(doc, with_fill_stub, autosar, gen_date)),
        (f"include/{ns}/contracts/thyme_types.h",
         emit_thyme_types(doc, autosar, gen_date)),
        (f"include/{ns}/contracts/contracts_json.h", emit_contracts_json(doc)),
        ("template/contracts_include.txt", f"{ns}/contracts/types.h\n"),
        # GEN-04: tells gen_graph (viz_ser emit) that contracts_json.h exists —
        # explicit template config instead of a filesystem probe of the app tree.
        ("template/contracts_json.txt", f"{ns}/contracts/contracts_json.h\n"),
        # GEN-08: schema-derived payload sizes for gen_graph's transport check.
        ("template/contract_sizes.txt", emit_contract_sizes(doc)),
        # ABI-09: runtime field layout for the package schema sidecar, plus the
        # explicit template marker so gen_graph wires it without probing the tree.
        (f"include/{ns}/contracts/contract_layout.h", emit_contract_layout(doc)),
        ("template/contracts_layout.txt",
         f"{ns}/contracts/contract_layout.h\n"),
    ]
    layout_checks = emit_layout_checks(doc)
    if layout_checks is not None:
        outputs.append((f"include/{ns}/contracts/layout_checks.h", layout_checks))
    return outputs


def scan_graph_cpp_types(path: str) -> list[str] | None:
    try:
        with open(path, encoding="utf-8") as f:
            types: list[str] = []
            for line in f:
                m = CPP_TYPE_RE.match(line)
                if m:
                    types.append(m.group(1))
            return types
    except OSError as e:
        error(f"cannot read graph {path}: {e}")
        return None


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Emit a thyme contracts header from a per-project datatypes.json"
    )
    parser.add_argument("--in", dest="in_file", required=True, metavar="FILE",
                        help="datatypes.json input")
    parser.add_argument("--out", dest="out_dir", required=True, metavar="DIR",
                        help="project root to emit into")
    parser.add_argument("--graph", metavar="GRAPH.yaml",
                        help="graph yaml to check cpp_type coverage against")
    parser.add_argument("--check", action="store_true",
                        help="validate only; write nothing")
    parser.add_argument("--force", action="store_true",
                        help="overwrite existing outputs")
    parser.add_argument("--with-fill-stub", action="store_true",
                        help="append the FillStub helper template")
    parser.add_argument("--autosar-types", action="store_true",
                        help="emit AUTOSAR (Platform_Types.h) field type spellings "
                             "(uint8/sint8/float32/boolean) and include "
                             "<namespace>/contracts/Platform_Types.h")
    parser.add_argument("--date", metavar="YYYY-MM-DD", default=None,
                        help="generation date for the types.h provenance comment "
                             "(default: today's date)")
    args = parser.parse_args()

    try:
        with open(args.in_file, encoding="utf-8") as f:
            doc = json.load(f, object_pairs_hook=_json_object)
    except OSError as e:
        error(f"cannot read {args.in_file}: {e}")
        return EXIT_ERROR
    except json.JSONDecodeError as e:
        error(f"cannot parse {args.in_file}: {e}")
        return EXIT_ERROR

    errors, warnings = validate(doc)

    if not errors and args.graph:
        graph_types = scan_graph_cpp_types(args.graph)
        if graph_types is None:
            return EXIT_ERROR
        defined = {t.get("name") for t in doc.get("types", []) if isinstance(t, dict)}
        for gt in graph_types:
            if gt not in defined:
                errors.append(f"graph cpp_type {gt!r} is missing from datatypes.json")

    if errors:
        for e in errors:
            error(e)
        return EXIT_ERROR

    for w in warnings:
        warn(w)

    out = Path(args.out_dir)
    gen_date = args.date or datetime.date.today().isoformat()
    outputs = plan_outputs(doc, args.with_fill_stub, args.autosar_types, gen_date)

    if args.check:
        for rel, _ in outputs:
            print(rel)
        return 0

    paths = [(out / rel, content) for rel, content in outputs]
    existing = [p for p, _ in paths if p.exists()]
    if existing and not args.force:
        for p in existing:
            error(f"refusing to overwrite {p} (use --force)")
        return EXIT_ERROR

    for p, content in paths:
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(content, encoding="utf-8")
        print(f"wrote {p}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
