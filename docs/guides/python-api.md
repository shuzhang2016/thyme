# Python parsing library (`thyme_py`)

`tools/inspect/thyme_py/` is a **stdlib-only, read-only** package parser: it does not link thyme, has no third-party dependencies, and reads `.thyme.bin` (v1–v4), the footer index, and `schema.json` (v1–v3) directly from Python. It is intended for offline forensics, CI scripts, and analysis tools that do not compile C++.

The authoritative definitions of the package format and ABI live in
[`adr/0005-thyme-bin-record-format-and-package-layout.md`](../design/adr/0005-thyme-bin-record-format-and-package-layout.md)
and [`include/thyme/record/thyme_bin.h`](../../include/thyme/record/thyme_bin.h);
this document describes only the Python-side API.

## 1. Quick start

The package source is in `tools/inspect/thyme_py/thyme_py/`; the parent directory `tools/inspect/thyme_py` is the import root:

```bash
cd tools/inspect/thyme_py
python3 -m thyme_py info /data/capture
python3 -m thyme_py dump /data/capture --limit 10
```

Or set the import path from any directory:

```bash
PYTHONPATH=/path/to/thyme/tools/inspect/thyme_py python3 -m thyme_py info /data/capture
```

```python
from thyme_py import ThymePackage

pkg = ThymePackage("/data/capture")
print(pkg.schema_state)                    # "ok" | "absent"
for item in pkg.iter_records():
    rec = item.record
    print(item.file, rec.kind, rec.mono_ns, rec.payload_size)

with pkg.open("small.tbin") as binary:
    print(binary.header.file_abi_version, binary.index_status)
    for rec in binary.iter_records():
        ...
```

Python 3.9+ is required (the code uses `from __future__ import annotations` and the standard-library modules `dataclasses/enum/struct/zlib`); no `pip install` is needed.

## 2. Supported range

| Item | Support |
|---|---|
| `.thyme.bin` v1 | streaming recording, no footer; parse sequentially to EOF |
| v2 | optional footer index; when a footer is present the record stream stops at `entries_offset` |
| v3 | `THYMANI1` manifest block (magic/version/reserved/CRC all verified), 40 B legacy registry |
| v4 | manifest block + 128 B `ThymeStructDesc` v2 (`layout_hash`/`channel`/`record_key`) |
| Header | 40 B `ThymeBinFileHeader`, up to 64 structs; registry entries are 40 B or 128 B depending on file version |
| Records | `data(0x01)`, `gap(0x02)`, `trigger(0x03)`, `session(0x04)`, each with CRC-32 |
| Footer index | v1 (`THYIDX1`), 32 B entry, 64 B footer: CRC, range, ordinal, monotonicity |
| `schema.json` | v1 / v2 / v3: `format/schema_version/partial/record_manifest(channels)/frame_header/records/fields`, plus `payload_profiles[]` in v3 |
| Field decoding | when `layout=known` and the field type is a scalar (`uint8..float64`), decode at `payload_offset + field.offset` |

The mapping between file version and registry entry size is a hard constraint: v1–v3 always use 40 B legacy entries, v4 always uses 128 B v2 entries (`KEYED_REGISTRY_VERSION`). A wrong size fails parsing immediately; it is never "guessed" into a different layout.

What it deliberately does not do:

- It does not guess field layouts. `layout=unavailable` (internal records such as `FrameIdx`, or hosts without codegen) can only slice raw bytes by `payload_size`.
- It does not guess an unknown field type (nested types, or spellings other than array element types) into some C++ type; `decode_record` skips them.
- It does not verify package-level integrity or signatures (that is the job of `thyme-replay integrity`); it only verifies the CRC and layout of records/index/manifest.
- It does not write files or modify packages.

## 3. API

### 3.1 `ThymeBinFile`

```python
ThymeBinFile(path, verify_crc=True)
```

Construction parses the header/registry/manifest/index; records are streamed on demand, so the whole file is never loaded into memory.

| Property | Meaning |
|---|---|
| `header.file_abi_version` / `device_id` / `session_id` / `start_mono_ns` / `struct_count` | file header |
| `registry` | `list[StructDesc]`: `struct_id/contract_version/payload_size/name` + v2 `layout_hash/channel/record_key` (empty/0 for legacy entries) |
| `manifest_status` | `"absent"` (v1/v2) or `"ok"` (v3/v4, CRC verified) |
| `manifest` | `ManifestBlock(manifest_version, manifest_hash)` or `None` |
| `index_status` | `"absent"` / `"ok"` / `"corrupt"` |
| `index_error` | reason when corrupt; otherwise `None` |
| `index_entries` / `index_footer` | verified seek points / footer summary |
| `data_offset` / `records_end` | absolute offset of the first record / end offset of the record stream |
| `file_size` | file size in bytes |

`StructDesc.key` is the schema-v2 canonical topology key: it takes `record_key` when non-empty, otherwise falls back to `channel`, and returns `None` when both are empty (legacy entries) — in that case you can only map by `struct_id` and must not assume the id means the same as `RecordStructId` in the current build.

Methods:

```python
iter_records(verify_crc=None, start=None, stop=None) -> Iterator[Record]
records(verify_crc=None) -> list[Record]
iter_from(mono_ns, verify_crc=None) -> Iterator[Record]   # O(log n) positioning when an index exists
struct(struct_id) -> StructDesc | None
summary() -> dict
close() / with ...
```

`Record` fields:

| Field | data | gap | trigger | session |
|---|---|---|---|---|
| `kind` | `"data"` | `"gap"` | `"trigger"` | `"session"` |
| `offset` / `next_offset` | absolute record start/end offsets | same | same | same |
| `mono_ns` | timestamp | timestamp | timestamp | `None` (not present in file) |
| `record_sequence` | data sequence number | 0 | `trigger_id` | `new_session_id` |
| `struct_id` | struct id | struct id | 0 | 0 |
| `source_sequence` | source sequence number | `missed_count` | 0 | 0 |
| `payload` / `payload_size` | raw bytes | `b""` | `b""` | `b""` |
| `reason` | — | gap reason | — | session reason |
| `missed_count` / `trigger_id` / `pre_roll_ns` / `post_roll_ns` / `session_id` | — | respective field | respective field | respective field |
| `crc_ok` | `True`/`None` | same | same | same |

With `verify_crc=False`, `crc_ok is None`: "not verified" is never disguised as "verified". A CRC mismatch raises `ThymeCrcError` (carrying `offset/expected/actual`); truncation, bad magic, or an unknown record type raises `ThymeFormatError`.

### 3.2 `ThymePackage`

```python
ThymePackage(path_or_file)
```

Directory: recursively collect all `*.tbin` files (sorted, relative paths, exposed as `files`), and read the root `schema.json`. Single file: contains only that file, with the sidecar taken from the `schema.json` in the same directory.

```python
pkg = ThymePackage("/data/capture")
pkg.kind            # "package" | "file"
pkg.files           # ["frames/cam_a.index.tbin", "meta.tbin", "small.tbin"]
pkg.schema          # PackageSchema | None
pkg.schema_state    # "ok" | "absent"

for item in pkg.iter_records():          # FileRecord(file, record)
    ...
pkg.decode(item)                         # data record -> {field: value}
pkg.info()                               # JSON shape used by the CLI info command
```

### 3.3 `schema.json`

```python
from thyme_py import load_schema, decode_record

schema = load_schema("/data/capture")     # directory or file; returns None if absent
rec = schema.by_id()[0]
rec.layout, rec.payload_offset, rec.contract_bytes, rec.fields
rec.channel, rec.record_key, rec.layout_hash          # v2 (empty/0 in v1)
schema.channels                           # v2 key table, sorted by struct_id
schema.by_key()["cam.pod16"]              # look up a record by canonical key
values = decode_record(schema, record)    # decodes only scalar fields with layout=known
```

`load_schema` validates `format="thyme-record-schema"`, `schema_version` (1, 2, or 3), `partial`, `record_manifest` (including `channels`), `frame_header`, the required fields of `records[]`, duplicate `struct_id`s, and so on. v2 additionally requires every record to carry `channel/record_key/layout_hash` and cross-checks that `channels[]` and `records[]` describe the same identities; v3 requires `payload_profiles[]` (an opaque decoder identifier whose parameters the core does not parse). v1 rejects v2-only fields, and v1/v2 reject a non-empty `payload_profiles`. When the structure is inconsistent it raises `ThymeSchemaError` rather than returning something half-trusted.

## 4. CLI

```
python -m thyme_py info <pkg-or-file> [--json] [--no-crc]
python -m thyme_py dump <pkg-or-file> [--limit N] [--hex] [--no-crc]
```

`info` text example:

```
path   : /data/capture (package)
schema : ok version=2 partial=false records=4 manifest=0x9914c81f74037d59 v1
small.tbin:
  version      : 4
  session_id   : 0x5151
  structs      : 0:Pod16/cv1/16B
  manifest     : 0x9914c81f74037d59 version=1
  records      : data=300 gap=0 trigger=0 session=0
  index        : ok blocks=5 stride=64 total_records=300
  mono_ns      : 1000001000 .. 1000299000
  offsets      : data=328 records_end=...
```

`info --json` emits a stable, machine-readable object:

```json
{"format":"thyme-python-info","version":1,"path":"...","kind":"package",
 "schema":{"state":"ok","version":2,"partial":false,
           "manifest":{"hash":"0x...","version":1},"records":4,"channels":2},
 "files":[{"path":"small.tbin","file_abi_version":4,
           "device_id":0,"session_id":20817,"start_mono_ns":1000000000,
           "structs":[{"struct_id":0,"name":"Pod16",
                       "contract_version":1,"payload_size":16,
                       "layout_hash":"0x031e5529cb6c1a1c",
                       "channel":"cam.pod16","record_key":"cam.pod16"}],
           "manifest":{"status":"ok","version":1,"hash":"0x..."},
           "index":{"status":"ok","error":null,"blocks":5,
                    "block_size":64,"total_records":300},
           "counts":{"data":300,"gap":0,"trigger":0,"session":0},
           "first_mono_ns":1000001000,"last_mono_ns":1000299000,
           "data_offset":328,"records_end":12345}]}
```

For legacy files/entries these fields are `null` (`layout_hash`/`channel`/`record_key`); v2 identity is never fabricated.

`dump` writes one JSON object per record (NDJSON); data payloads are base64 by default and hexadecimal with `--hex`; `gap/trigger/session` emit their respective scalar fields. `--limit` stops at the iteration layer, without loading the whole file first.

Exit codes: 0 success; 1 file/format/CRC error (stderr lines start with `thyme_py: error:`); 2 argparse usage error.

## 5. Tests

`test_thyme_py` uses `tests/thyme_py_fixture.cpp` to generate packages with the production C++ writer: a v4 package (keyed registry + manifest + footer index + `schema.json` v2 with known field layouts and a channel key table), plus v3/v2/v1 legacy packages (40 B registry, optional manifest/index). `unittest` then covers header/registry/manifest, the index, every record type, CRC (including mandatory errors after tampering), the sidecar (v2 identity cross-validation and v1 compatibility), field decoding, the package facade, and the CLI. To run it manually:

```bash
./build/thyme_py_fixture /tmp/thyme-fixtures
PYTHONPATH=tools/inspect/thyme_py python3 tools/inspect/thyme_py/tests/run_tests.py \
    build/thyme_py_fixture
```
