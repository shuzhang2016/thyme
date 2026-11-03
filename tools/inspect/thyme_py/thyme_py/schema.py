"""Reader for the ABI-09/ABI-11 ``schema.json`` package sidecar.

The sidecar describes the decode contract of the build that produced the
package (see ``include/thyme/record/record_schema.h``). It is optional for
legacy packages and only trustworthy when it can be parsed as the documented
shape; this module therefore validates the structure it needs and raises
:class:`ThymeSchemaError` on anything inconsistent instead of guessing.

Schema version 2 adds the ABI-11 identity triple: ``record_manifest.channels``
plus per-record ``channel``/``record_key``/``layout_hash``. Version 3 adds
``payload_profiles[]`` (REC-27 opaque big-frame decoder identity). Version 1
and 2 sidecars keep being readable.

Field decoding is best-effort by design: a record with ``layout=unavailable``
must be treated as raw bytes, and unknown field type spellings are skipped
rather than guessed.
"""

from __future__ import annotations

import json
import os
import struct
from dataclasses import dataclass
from typing import Any, Optional

from .binary import Record, ThymeError


class ThymeSchemaError(ThymeError):
    """``schema.json`` is present but not a usable sidecar."""


SCHEMA_FORMAT = "thyme-record-schema"
# Version 1: ABI-09 (no record identity). Version 2: ABI-11 adds
# record_manifest.channels and the per-record identity triple.
# Version 3: REC-27 payload_profiles[].
SCHEMA_VERSION = 3
SCHEMA_VERSION_MIN = 1
SCHEMA_FILE = "schema.json"

# ABI-11 identity text limits (C++ char[] capacities, NUL-terminated).
CHANNEL_MAX_BYTES = 32
RECORD_KEY_MAX_BYTES = 48
PROFILE_MAX_BYTES = 56

# ABI-03 datatypes.json spellings that can be decoded without any codegen.
_TYPE_FORMATS = {
    "bool": "?",
    "uint8": "B",
    "int8": "b",
    "uint16": "H",
    "int16": "h",
    "uint32": "I",
    "int32": "i",
    "uint64": "Q",
    "int64": "q",
    "float32": "f",
    "float64": "d",
}


@dataclass(frozen=True)
class FieldDesc:
    name: str
    type: str
    offset: int
    size: int
    count: int
    stride: int


@dataclass(frozen=True)
class RecordDesc:
    struct_id: int
    name: str
    contract_version: int
    payload_size: int
    frame_header_bytes: int
    layout: str
    payload_offset: Optional[int]
    contract_bytes: Optional[int]
    fields: tuple[FieldDesc, ...]
    # ABI-11 (schema_version 2): empty for a legacy descriptor.
    channel: str = ""
    record_key: str = ""
    layout_hash: int = 0

    @property
    def known(self) -> bool:
        return self.layout == "known" and self.fields is not None

    @property
    def key(self) -> Optional[str]:
        """Canonical topology key: ``record_key``, else ``channel``."""
        return self.record_key or self.channel or None


@dataclass(frozen=True)
class ChannelDesc:
    """One ``record_manifest.channels`` entry (ABI-11, schema_version 2)."""

    struct_id: int
    channel: str
    record_key: str
    contract_version: int
    payload_size: int
    layout_hash: int


@dataclass(frozen=True)
class PayloadProfileDesc:
    """One ``payload_profiles`` entry (REC-27, schema_version 3)."""

    channel: str
    profile: str  # empty means JSON null
    version: int
    params_digest: int  # 0 means JSON null / no params


@dataclass(frozen=True)
class PackageSchema:
    path: str
    schema_version: int
    partial: bool
    manifest_hash: Optional[int]
    manifest_version: Optional[int]
    frame_header_bytes: int
    records: tuple[RecordDesc, ...]
    # ABI-11 key table, ordered by struct_id; empty on a v1 sidecar.
    channels: tuple[ChannelDesc, ...] = ()
    payload_profiles: tuple[PayloadProfileDesc, ...] = ()

    def by_id(self) -> dict[int, RecordDesc]:
        return {r.struct_id: r for r in self.records}

    def by_key(self) -> dict[str, RecordDesc]:
        """Map canonical record keys to descriptors (v2 identity only)."""
        return {r.key: r for r in self.records if r.key is not None}


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise ThymeSchemaError(message)


def _uint(value: Any, name: str, maximum: Optional[int] = None) -> int:
    _require(isinstance(value, int) and not isinstance(value, bool),
             f"{name} must be an integer")
    _require(value >= 0, f"{name} must be non-negative")
    if maximum is not None:
        _require(value <= maximum, f"{name} is out of range")
    return value


def _field(raw: Any, where: str) -> FieldDesc:
    _require(isinstance(raw, dict), f"{where}.fields entries must be objects")
    for key in ("name", "type", "offset", "size", "count", "stride"):
        _require(key in raw, f"{where} field is missing '{key}'")
    _require(isinstance(raw["name"], str), f"{where}.name must be a string")
    _require(isinstance(raw["type"], str), f"{where}.type must be a string")
    return FieldDesc(
        name=raw["name"],
        type=raw["type"],
        offset=_uint(raw["offset"], f"{where}.offset"),
        size=_uint(raw["size"], f"{where}.size"),
        count=_uint(raw["count"], f"{where}.count"),
        stride=_uint(raw["stride"], f"{where}.stride"),
    )


def _text(raw: Any, where: str, maximum: int) -> str:
    _require(isinstance(raw, str), f"{where} must be a string")
    _require(len(raw.encode("utf-8")) < maximum, f"{where} is too long")
    return raw


def _optional_text(raw: Any, where: str, maximum: int) -> str:
    if raw is None:
        return ""
    return _text(raw, where, maximum)


def _layout_hash(raw: Any, where: str) -> int:
    """Decode an ABI-11 ``0x<16 hex>`` layout hash; ``None``/absent means 0."""
    if raw is None:
        return 0
    _require(isinstance(raw, str) and raw.startswith("0x"),
             f"{where} must be null or a 0x-prefixed string")
    try:
        value = int(raw, 16)
    except ValueError as exc:
        raise ThymeSchemaError(
            f"{where} is not a hexadecimal integer") from exc
    _require(0 <= value <= 0xFFFFFFFFFFFFFFFF, f"{where} is out of range")
    return value


def _record(raw: Any, idx: int, schema_version: int) -> RecordDesc:
    where = f"records[{idx}]"
    _require(isinstance(raw, dict), f"{where} must be an object")
    required = ("struct_id", "name", "contract_version", "payload_size",
                "frame_header_bytes", "layout", "payload_offset",
                "contract_bytes", "fields")
    if schema_version >= 2:
        # ABI-11: the identity triple is mandatory in a v2 sidecar.
        required += ("channel", "record_key", "layout_hash")
    for key in required:
        _require(key in raw, f"{where} is missing '{key}'")
    layout = raw["layout"]
    _require(layout in ("known", "unavailable"),
             f"{where}.layout must be 'known' or 'unavailable'")
    fields_raw = raw["fields"]
    if layout == "known":
        _require(isinstance(fields_raw, list), f"{where}.fields must be a list")
        _require(raw["payload_offset"] is not None and
                 raw["contract_bytes"] is not None,
                 f"{where} known layout needs payload_offset/contract_bytes")
        fields = tuple(_field(f, f"{where}.fields[{i}]")
                       for i, f in enumerate(fields_raw))
    else:
        _require(fields_raw is None, f"{where}.fields must be null")
        _require(raw["payload_offset"] is None and
                 raw["contract_bytes"] is None,
                 f"{where} unavailable layout must not carry an offset")
        fields = ()
    channel = _optional_text(raw.get("channel"), f"{where}.channel",
                             CHANNEL_MAX_BYTES)
    record_key = _optional_text(raw.get("record_key"), f"{where}.record_key",
                                RECORD_KEY_MAX_BYTES)
    if channel and not record_key:
        record_key = channel  # canonical key defaults to the channel
    return RecordDesc(
        struct_id=_uint(raw["struct_id"], f"{where}.struct_id", 0xFFFF),
        name=raw["name"],
        contract_version=_uint(raw["contract_version"],
                               f"{where}.contract_version", 0xFFFF),
        payload_size=_uint(raw["payload_size"], f"{where}.payload_size"),
        frame_header_bytes=_uint(raw["frame_header_bytes"],
                                 f"{where}.frame_header_bytes"),
        layout=layout,
        payload_offset=(None if raw["payload_offset"] is None
                        else _uint(raw["payload_offset"],
                                   f"{where}.payload_offset")),
        contract_bytes=(None if raw["contract_bytes"] is None
                        else _uint(raw["contract_bytes"],
                                   f"{where}.contract_bytes")),
        fields=fields,
        channel=channel,
        record_key=record_key,
        layout_hash=_layout_hash(raw.get("layout_hash"),
                                 f"{where}.layout_hash"),
    )


def _channel(raw: Any, idx: int) -> ChannelDesc:
    where = f"record_manifest.channels[{idx}]"
    _require(isinstance(raw, dict), f"{where} must be an object")
    for key in ("struct_id", "channel", "record_key", "contract_version",
                "payload_size", "layout_hash"):
        _require(key in raw, f"{where} is missing '{key}'")
    channel = _text(raw["channel"], f"{where}.channel", CHANNEL_MAX_BYTES)
    _require(channel != "", f"{where}.channel must not be empty")
    record_key = _optional_text(raw["record_key"], f"{where}.record_key",
                                RECORD_KEY_MAX_BYTES)
    return ChannelDesc(
        struct_id=_uint(raw["struct_id"], f"{where}.struct_id", 0xFFFF),
        channel=channel,
        record_key=record_key or channel,
        contract_version=_uint(raw["contract_version"],
                               f"{where}.contract_version", 0xFFFF),
        payload_size=_uint(raw["payload_size"], f"{where}.payload_size"),
        layout_hash=_layout_hash(raw["layout_hash"],
                                 f"{where}.layout_hash"),
    )


def _parse_hash(raw: Any) -> Optional[int]:
    if raw is None:
        return None
    _require(isinstance(raw, str) and raw.startswith("0x"),
             "record_manifest.hash must be null or a 0x-prefixed string")
    try:
        return int(raw, 16)
    except ValueError as exc:
        raise ThymeSchemaError(
            "record_manifest.hash is not a hexadecimal integer") from exc


def _parse_channels(manifest: dict[str, Any],
                    schema_version: int) -> tuple[ChannelDesc, ...]:
    """Decode ``record_manifest.channels`` for its schema version.

    v2 requires the array (null is rejected); v1 only defines null, mirroring
    the C++ reader so a sidecar cannot mean two things depending on its
    ``schema_version``.
    """
    _require("channels" in manifest,
             "record_manifest is missing 'channels'")
    raw = manifest["channels"]
    if schema_version < 2:
        _require(raw is None,
                 "schema_version 1 cannot carry record_manifest.channels")
        return ()
    _require(isinstance(raw, list),
             "schema_version 2 requires the record_manifest.channels array")
    return tuple(_channel(item, i) for i, item in enumerate(raw))


def _payload_profile(raw: Any, idx: int) -> PayloadProfileDesc:
    where = f"payload_profiles[{idx}]"
    _require(isinstance(raw, dict), f"{where} must be an object")
    for key in ("channel", "profile", "profile_version", "params_digest"):
        _require(key in raw, f"{where} is missing '{key}'")
    channel = _text(raw["channel"], f"{where}.channel", CHANNEL_MAX_BYTES)
    _require(channel != "", f"{where}.channel must not be empty")
    profile = _optional_text(raw["profile"], f"{where}.profile",
                             PROFILE_MAX_BYTES)
    version = _uint(raw["profile_version"], f"{where}.profile_version")
    digest = _layout_hash(raw["params_digest"], f"{where}.params_digest")
    if not profile:
        _require(version == 0 and digest == 0,
                 f"{where} empty profile cannot carry version or digest")
    return PayloadProfileDesc(channel=channel, profile=profile,
                              version=version, params_digest=digest)


def _parse_payload_profiles(raw: dict[str, Any],
                            schema_version: int) -> tuple[PayloadProfileDesc, ...]:
    if schema_version >= 3:
        _require("payload_profiles" in raw,
                 "schema_version 3 requires payload_profiles")
    elif "payload_profiles" not in raw:
        return ()
    items = raw["payload_profiles"]
    _require(isinstance(items, list), "payload_profiles must be a list")
    profiles = tuple(_payload_profile(item, i) for i, item in enumerate(items))
    if schema_version < 3:
        _require(not profiles,
                 f"schema_version {schema_version} cannot carry "
                 "payload_profiles")
    seen: set[str] = set()
    for profile in profiles:
        _require(profile.channel not in seen,
                 f"duplicate payload_profiles channel '{profile.channel}'")
        seen.add(profile.channel)
    return profiles


def _cross_check_identity(records: tuple[RecordDesc, ...],
                          channels: tuple[ChannelDesc, ...]) -> None:
    """Reject a v2 sidecar whose two identity views disagree."""
    by_id = {r.struct_id: r for r in records}
    seen_channels: set[str] = set()
    seen_keys: set[str] = set()
    previous_id = -1
    for channel in channels:
        _require(channel.struct_id > previous_id,
                 "record_manifest.channels is not strictly ordered by "
                 "struct_id")
        previous_id = channel.struct_id
        record = by_id.get(channel.struct_id)
        _require(record is not None,
                 f"record_manifest.channels references unknown struct_id "
                 f"{channel.struct_id}")
        _require(channel.channel not in seen_channels,
                 f"duplicate channel '{channel.channel}'")
        _require(channel.record_key not in seen_keys,
                 f"duplicate record_key '{channel.record_key}'")
        seen_channels.add(channel.channel)
        seen_keys.add(channel.record_key)
        _require(record.channel == channel.channel and
                 record.record_key == channel.record_key and
                 record.contract_version == channel.contract_version and
                 record.payload_size == channel.payload_size and
                 record.layout_hash == channel.layout_hash,
                 f"record_manifest.channels disagrees with record "
                 f"{channel.struct_id}")
    for record in records:
        if not record.channel:
            continue
        _require(any(c.struct_id == record.struct_id for c in channels),
                 f"record '{record.name}' has a channel but no "
                 "record_manifest.channels entry")


def load_schema(path: os.PathLike[str] | str) -> Optional[PackageSchema]:
    """Read ``schema.json`` from a file or package directory.

    Returns ``None`` when no sidecar exists (legacy package). Raises
    :class:`ThymeSchemaError` when the file exists but is not the documented
    ABI-09/ABI-11/REC-27 shape (``schema_version`` 1, 2 or 3).
    """
    candidate = os.fspath(path)
    if os.path.isdir(candidate):
        candidate = os.path.join(candidate, SCHEMA_FILE)
        if not os.path.exists(candidate):
            return None
    try:
        with open(candidate, "r", encoding="utf-8") as fp:
            raw = json.load(fp)
    except FileNotFoundError:
        return None
    except (OSError, json.JSONDecodeError) as exc:
        raise ThymeSchemaError(f"cannot read {candidate}: {exc}") from exc

    _require(isinstance(raw, dict), "schema.json must contain an object")
    _require(raw.get("format") == SCHEMA_FORMAT,
             f"schema.json format must be {SCHEMA_FORMAT!r}")
    version = _uint(raw.get("schema_version"), "schema_version")
    _require(SCHEMA_VERSION_MIN <= version <= SCHEMA_VERSION,
             f"unsupported schema_version {version}")
    partial = raw.get("partial")
    _require(isinstance(partial, bool), "partial must be true/false")
    frame = raw.get("frame_header")
    _require(isinstance(frame, dict), "frame_header must be an object")
    manifest = raw.get("record_manifest")
    _require(isinstance(manifest, dict), "record_manifest must be an object")
    channels = _parse_channels(manifest, version)
    records_raw = raw.get("records")
    _require(isinstance(records_raw, list), "records must be a list")

    records = tuple(_record(item, i, version)
                    for i, item in enumerate(records_raw))
    ids = [r.struct_id for r in records]
    _require(len(set(ids)) == len(ids), "duplicate struct_id in records")
    if version < 2:
        # A v1 sidecar must not smuggle ABI-11 identity fields in.
        for record in records:
            _require(not record.channel and not record.record_key and
                     record.layout_hash == 0,
                     f"schema_version 1 record '{record.name}' carries "
                     "ABI-11 identity fields")
    else:
        _cross_check_identity(records, channels)
    profiles = _parse_payload_profiles(raw, version)

    return PackageSchema(
        path=os.path.abspath(candidate),
        schema_version=version,
        partial=partial,
        manifest_hash=_parse_hash(manifest.get("hash")),
        manifest_version=(None if manifest.get("version") is None
                          else _uint(manifest["version"],
                                     "record_manifest.version", 0xFFFF)),
        frame_header_bytes=_uint(frame.get("bytes"),
                                 "frame_header.bytes"),
        records=records,
        channels=channels,
        payload_profiles=profiles,
    )


def decode_record(schema: PackageSchema, record: Record) -> dict[str, Any]:
    """Decode a data record's known fields from its payload.

    Raises :class:`ThymeSchemaError` when the record's struct is not in the
    sidecar, its layout is unavailable, or the payload is too short. Unknown
    field type spellings are omitted from the result (never guessed).
    """
    if record.kind != "data":
        raise ThymeSchemaError("only data records carry struct payloads")
    desc = schema.by_id().get(record.struct_id)
    if desc is None:
        raise ThymeSchemaError(
            f"struct {record.struct_id} is not described by the sidecar")
    if not desc.known:
        raise ThymeSchemaError(
            f"struct {record.struct_id} ({desc.name}) has no field layout")
    if desc.payload_offset is None or desc.contract_bytes is None:
        raise ThymeSchemaError(
            f"struct {record.struct_id} ({desc.name}) has an incomplete layout")
    if len(record.payload) < desc.payload_offset + desc.contract_bytes:
        raise ThymeSchemaError(
            f"payload of struct {record.struct_id} is shorter than the "
            "sidecar contract")
    out: dict[str, Any] = {}
    base = desc.payload_offset
    for field in desc.fields:
        fmt = _TYPE_FORMATS.get(field.type)
        if fmt is None:
            continue
        if field.count <= 1:
            value = struct.unpack_from("<" + fmt, record.payload,
                                       base + field.offset)[0]
        else:
            value = [
                struct.unpack_from("<" + fmt, record.payload,
                                   base + field.offset + i * field.stride)[0]
                for i in range(field.count)
            ]
        out[field.name] = value
    return out


def schema_summary(schema: Optional[PackageSchema]) -> dict[str, Any]:
    if schema is None:
        return {"state": "absent"}
    return {
        "state": "ok",
        "path": schema.path,
        "version": schema.schema_version,
        "partial": schema.partial,
        "manifest": (None if schema.manifest_hash is None else {
            "hash": f"0x{schema.manifest_hash:016x}",
            "version": schema.manifest_version,
        }),
        "records": len(schema.records),
        "channels": len(schema.channels),
        "payload_profiles": len(schema.payload_profiles),
    }
