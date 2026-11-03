"""Pure-stdlib reader for the ``.thyme.bin`` capture format.

The parser mirrors the C++ reader in ``src/record/thyme_bin.cpp`` and the
on-disk ABI in ``include/thyme/record/thyme_bin.h``:

* file versions 1 (stream, no footer), 2 (optional REC-04 footer index),
  3 (ABI-10 record-manifest block after the struct registry) and 4 (ABI-11
  keyed registry);
* 40-byte file header, 40-byte legacy ``ThymeStructDesc`` entries (file
  versions 1-3) or 128-byte version-2 entries (file version 4) carrying
  ``layout_hash``/``channel``/``record_key``, the 32-byte ``THYMANI1``
  manifest block, 32-byte footer index entries and the 64-byte footer;
* per-record CRC-32 (IEEE) over the exact fields the C++ encoder covers.

Everything is read-only and allocation-bounded by the on-disk payload ceiling;
a corrupt record raises :class:`ThymeCrcError` instead of being silently
dropped (the C++ ``ThymeBinReader::ReadResult::kCorrupt`` equivalent).
"""

from __future__ import annotations

import bisect
import os
import struct
import zlib
from dataclasses import dataclass
from enum import IntEnum
from typing import Iterator, Optional


THYME_BIN_MAGIC = b"THYM"
MANIFEST_MAGIC = b"THYMANI1"
INDEX_MAGIC = b"THYIDX1\x00"
MAX_STRUCT_COUNT = 64
MAX_PAYLOAD_SIZE = 512 * 1024
INDEX_BLOCK_SIZE = 64
# 1..3 use the legacy 40-byte registry entry; 4 is ABI-11 (128-byte entry).
SUPPORTED_VERSIONS = (1, 2, 3, 4)
KEYED_REGISTRY_VERSION = 4
MANIFEST_BLOCK_VERSION = 3

FILE_HEADER_BYTES = 40
# Legacy (version-1) ThymeStructDesc: the on-disk entry for file versions 1-3.
STRUCT_DESC_BYTES = 40
# ABI-11 (version-2) ThymeStructDesc: the on-disk entry for file version 4.
KEYED_STRUCT_DESC_BYTES = 128
MANIFEST_BLOCK_BYTES = 32
INDEX_ENTRY_BYTES = 32
INDEX_FOOTER_BYTES = 64
DATA_HEADER_BYTES = 32


class ThymeError(Exception):
    """Base class for thyme reader errors."""


class ThymeFormatError(ThymeError):
    """The file does not follow the documented on-disk layout."""


class ThymeCrcError(ThymeFormatError):
    """A record/index/manifest CRC does not match the stored bytes."""

    def __init__(self, message: str, offset: int, expected: int, actual: int):
        super().__init__(f"{message} at offset {offset}: "
                         f"stored=0x{actual:08x} computed=0x{expected:08x}")
        self.offset = offset
        self.expected = expected
        self.actual = actual


class RecordType(IntEnum):
    DATA = 0x01
    GAP = 0x02
    TRIGGER = 0x03
    SESSION = 0x04


@dataclass(frozen=True)
class FileHeader:
    file_abi_version: int
    device_id: int
    session_id: int
    start_mono_ns: int
    struct_count: int
    reserved0: int = 0
    reserved1: int = 0


@dataclass(frozen=True)
class StructDesc:
    """One struct registry entry.

    ``layout_hash``/``channel``/``record_key`` are ABI-11 identity fields.
    They are empty/0 for a legacy (file version 1-3) entry, which is the
    documented fallback: such a file maps by ``struct_id``, not by key.
    """

    struct_id: int
    contract_version: int
    payload_size: int
    name: str
    layout_hash: int = 0
    channel: str = ""
    record_key: str = ""

    @property
    def key(self) -> Optional[str]:
        """Canonical topology key: ``record_key``, else ``channel``."""
        return self.record_key or self.channel or None


@dataclass(frozen=True)
class ManifestBlock:
    manifest_version: int
    manifest_hash: int


@dataclass(frozen=True)
class IndexEntry:
    offset: int
    mono_ns: int
    record_sequence: int
    record_index: int


@dataclass(frozen=True)
class IndexFooter:
    index_version: int
    block_size: int
    block_count: int
    entries_offset: int
    data_offset: int
    total_records: int
    first_mono_ns: int
    last_mono_ns: int


@dataclass
class Record:
    """One record from the stream.

    ``crc_ok`` is ``None`` when CRC verification was disabled. Session rows
    carry no timestamp in the file (``mono_ns`` is ``None``); the C++ exporters
    stamp them with the last observed time.
    """

    kind: str
    offset: int
    next_offset: int
    record_sequence: int = 0
    struct_id: int = 0
    mono_ns: Optional[int] = None
    source_sequence: int = 0
    payload_size: int = 0
    payload: bytes = b""
    crc32: int = 0
    crc_ok: Optional[bool] = None
    reserved: int = 0
    # GAP
    reason: Optional[int] = None
    missed_count: Optional[int] = None
    # TRIGGER
    trigger_id: Optional[int] = None
    pre_roll_ns: Optional[int] = None
    post_roll_ns: Optional[int] = None
    # SESSION
    session_id: Optional[int] = None


def _crc32(*chunks: bytes, seed: int = 0) -> int:
    crc = seed
    for chunk in chunks:
        crc = zlib.crc32(chunk, crc)
    return crc & 0xFFFFFFFF


def _u16(data: bytes, offset: int) -> int:
    return struct.unpack_from("<H", data, offset)[0]


def _u32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def _u64(data: bytes, offset: int) -> int:
    return struct.unpack_from("<Q", data, offset)[0]


def _decode_name(raw: bytes) -> str:
    return raw.split(b"\x00", 1)[0].decode("utf-8", "replace")


def _validate_header(data: bytes) -> FileHeader:
    if len(data) != FILE_HEADER_BYTES:
        raise ThymeFormatError("truncated .thyme.bin header")
    if data[:4] != THYME_BIN_MAGIC:
        raise ThymeFormatError("bad .thyme.bin magic")
    version = _u16(data, 4)
    if version not in SUPPORTED_VERSIONS:
        raise ThymeFormatError(f"unsupported .thyme.bin version {version}")
    struct_count = _u32(data, 32)
    if struct_count > MAX_STRUCT_COUNT:
        raise ThymeFormatError(f"struct_count {struct_count} exceeds "
                               f"{MAX_STRUCT_COUNT}")
    return FileHeader(
        file_abi_version=version,
        device_id=_u32(data, 8),
        session_id=_u64(data, 16),
        start_mono_ns=_u64(data, 24),
        struct_count=struct_count,
        reserved0=_u16(data, 6),
        reserved1=_u32(data, 36),
    )


def _validate_manifest(data: bytes) -> ManifestBlock:
    if len(data) != MANIFEST_BLOCK_BYTES:
        raise ThymeFormatError("truncated record-manifest block")
    if data[:8] != MANIFEST_MAGIC:
        raise ThymeFormatError("bad record-manifest magic")
    version = _u16(data, 8)
    if version == 0:
        raise ThymeFormatError("unset record-manifest version")
    if _u16(data, 10) != 0 or _u32(data, 12) != 0 or _u32(data, 24) != 0:
        raise ThymeFormatError("non-zero reserved field in manifest block")
    stored = _u32(data, 28)
    computed = _crc32(data[:28])
    if stored != computed:
        raise ThymeCrcError("record-manifest CRC mismatch", 0, computed, stored)
    return ManifestBlock(manifest_version=version,
                         manifest_hash=_u64(data, 16))


class ThymeBinFile:
    """Read-only view of one ``.thyme.bin`` file.

    ``ThymeBinFile(path)`` parses the header, registry, manifest and optional
    footer index eagerly; records are streamed by :meth:`iter_records`, so the
    file is never loaded into memory as a whole.
    """

    def __init__(self, path: os.PathLike[str] | str,
                 verify_crc: bool = True):
        self.path = os.fspath(path)
        self.verify_crc = verify_crc
        self._fp = open(self.path, "rb")
        try:
            self.file_size = os.fstat(self._fp.fileno()).st_size
            self._parse_preamble()
            self._load_index()
        except Exception:
            self._fp.close()
            raise

    # -- lifecycle ---------------------------------------------------------
    def close(self) -> None:
        if not self._fp.closed:
            self._fp.close()

    def __enter__(self) -> "ThymeBinFile":
        return self

    def __exit__(self, *exc_info) -> None:
        self.close()

    def __repr__(self) -> str:
        return (f"ThymeBinFile({self.path!r}, v{self.header.file_abi_version}, "
                f"structs={len(self.registry)})")

    # -- preamble ----------------------------------------------------------
    def _parse_preamble(self) -> None:
        self._fp.seek(0)
        header = _validate_header(self._fp.read(FILE_HEADER_BYTES))
        self.header = header
        registry = []
        keyed = header.file_abi_version >= KEYED_REGISTRY_VERSION
        entry_bytes = KEYED_STRUCT_DESC_BYTES if keyed else STRUCT_DESC_BYTES
        for _ in range(header.struct_count):
            raw = self._fp.read(entry_bytes)
            if len(raw) != entry_bytes:
                raise ThymeFormatError("truncated struct registry")
            struct_id, contract_version, payload_size, name = \
                struct.unpack_from("<HHI32s", raw, 0)
            if contract_version == 0:
                raise ThymeFormatError(
                    f"struct {struct_id} has contract_version 0")
            if keyed:
                layout_hash = _u64(raw, 40)
                channel = _decode_name(raw[48:80])
                record_key = _decode_name(raw[80:128])
            else:
                # Legacy entry: ABI-11 fields stay unset (map by struct_id).
                layout_hash, channel, record_key = 0, "", ""
            registry.append(StructDesc(struct_id, contract_version,
                                       payload_size, _decode_name(name),
                                       layout_hash, channel, record_key))
        self.registry = registry
        ids = [d.struct_id for d in registry]
        if len(set(ids)) != len(ids):
            raise ThymeFormatError("duplicate struct_id in registry")

        self.manifest: Optional[ManifestBlock] = None
        self.manifest_status = "absent"
        if header.file_abi_version >= MANIFEST_BLOCK_VERSION:
            raw = self._fp.read(MANIFEST_BLOCK_BYTES)
            try:
                self.manifest = _validate_manifest(raw)
            except ThymeFormatError as exc:
                raise ThymeFormatError(
                    f"version {header.file_abi_version} file without a valid "
                    f"manifest block: {exc}"
                ) from exc
            self.manifest_status = "ok"
        self.data_offset = self._fp.tell()

    # -- footer index ------------------------------------------------------
    def _load_index(self) -> None:
        self.index_status = "absent"
        self.index_error: Optional[str] = None
        self.index_entries: list[IndexEntry] = []
        self.index_footer: Optional[IndexFooter] = None
        self.records_end = self.file_size
        if self.file_size - self.data_offset < INDEX_FOOTER_BYTES:
            return
        footer_offset = self.file_size - INDEX_FOOTER_BYTES
        self._fp.seek(footer_offset)
        raw = self._fp.read(INDEX_FOOTER_BYTES)
        if raw[:8] != INDEX_MAGIC:
            return  # no footer: valid stream capture

        def corrupt(why: str) -> None:
            self.index_status = "corrupt"
            self.index_error = why

        index_version = _u16(raw, 8)
        reserved0 = _u16(raw, 10)
        block_size = _u32(raw, 12)
        block_count = _u32(raw, 16)
        stored_crc = _u32(raw, 20)
        entries_offset = _u64(raw, 24)
        data_offset = _u64(raw, 32)
        total_records = _u64(raw, 40)
        first_mono = _u64(raw, 48)
        last_mono = _u64(raw, 56)

        if index_version != 1:
            corrupt(f"unsupported index version {index_version}")
            return
        if reserved0 != 0:
            corrupt("non-zero reserved field in index footer")
            return
        if block_size != INDEX_BLOCK_SIZE:
            corrupt(f"unsupported index block size {block_size}")
            return
        zeroed = raw[:20] + b"\x00\x00\x00\x00" + raw[24:]
        if _crc32(zeroed) != stored_crc:
            corrupt("index footer CRC mismatch")
            return
        if data_offset != self.data_offset:
            corrupt("index data offset mismatch")
            return
        if not (self.data_offset <= entries_offset <= footer_offset):
            corrupt("index entries offset out of range")
            return
        if footer_offset - entries_offset != block_count * INDEX_ENTRY_BYTES:
            corrupt("index extent mismatch")
            return

        entries: list[IndexEntry] = []
        self._fp.seek(entries_offset)
        blob = self._fp.read(block_count * INDEX_ENTRY_BYTES)
        if len(blob) != block_count * INDEX_ENTRY_BYTES:
            corrupt("short index read")
            return
        prev_offset = prev_mono = prev_index = 0
        for i in range(block_count):
            off = i * INDEX_ENTRY_BYTES
            offset = _u64(blob, off)
            mono = _u64(blob, off + 8)
            record_sequence = _u64(blob, off + 16)
            record_index = _u32(blob, off + 24)
            stored = _u32(blob, off + 28)
            if _crc32(blob[off:off + 28]) != stored:
                corrupt(f"index entry {i} CRC mismatch")
                return
            if not (self.data_offset <= offset < entries_offset):
                corrupt(f"index entry {i} offset out of range")
                return
            if record_index % block_size != 0 or record_index >= total_records:
                corrupt(f"index entry {i} record ordinal mismatch")
                return
            if i and (offset <= prev_offset or record_index <= prev_index or
                      mono < prev_mono):
                corrupt("index entries not monotonic")
                return
            prev_offset, prev_mono, prev_index = offset, mono, record_index
            entries.append(IndexEntry(offset, mono, record_sequence,
                                      record_index))

        self.index_entries = entries
        self.index_footer = IndexFooter(index_version, block_size, block_count,
                                        entries_offset, data_offset,
                                        total_records, first_mono, last_mono)
        self.records_end = entries_offset
        self.index_status = "ok"

    # -- records -----------------------------------------------------------
    def _read_exact(self, n: int, offset: Optional[int] = None) -> bytes:
        data = self._fp.read(n)
        if len(data) != n:
            where = self._fp.tell() if offset is None else offset
            raise ThymeFormatError(f"truncated record at offset {where}")
        return data

    def _parse_record(self, offset: int,
                      verify_crc: bool) -> Record:
        self._fp.seek(offset)
        type_byte = self._fp.read(1)
        if not type_byte:
            raise ThymeFormatError(f"unexpected EOF at offset {offset}")
        pad = self._read_exact(3)
        kind_byte = type_byte[0]

        if kind_byte == RecordType.DATA:
            body = self._read_exact(DATA_HEADER_BYTES - 4)
            record_sequence, struct_id, reserved, mono_ns, source_sequence, \
                payload_size = struct.unpack("<QHHQII", body)
            if payload_size > MAX_PAYLOAD_SIZE:
                raise ThymeFormatError(
                    f"payload_size {payload_size} exceeds {MAX_PAYLOAD_SIZE} "
                    f"at offset {offset}")
            payload = self._read_exact(payload_size) if payload_size else b""
            stored = struct.unpack("<I", self._read_exact(4))[0]
            computed = _crc32(type_byte, struct.pack("<Q", record_sequence),
                              struct.pack("<H", struct_id),
                              struct.pack("<Q", mono_ns),
                              struct.pack("<I", source_sequence),
                              struct.pack("<I", payload_size), payload)
            end = self._fp.tell()
            rec = Record(kind="data", offset=offset, next_offset=end,
                         record_sequence=record_sequence, struct_id=struct_id,
                         mono_ns=mono_ns, source_sequence=source_sequence,
                         payload_size=payload_size, payload=payload,
                         crc32=stored, reserved=reserved)
        elif kind_byte == RecordType.GAP:
            data = self._read_exact(16)
            struct_id, reason, mono_ns, missed_count = struct.unpack(
                "<HHQI", data)
            stored = struct.unpack("<I", self._read_exact(4))[0]
            computed = _crc32(type_byte, struct.pack("<H", struct_id),
                              struct.pack("<H", reason),
                              struct.pack("<Q", mono_ns),
                              struct.pack("<I", missed_count))
            end = self._fp.tell()
            rec = Record(kind="gap", offset=offset, next_offset=end,
                         struct_id=struct_id, mono_ns=mono_ns,
                         source_sequence=missed_count, crc32=stored,
                         reason=reason, missed_count=missed_count)
        elif kind_byte == RecordType.TRIGGER:
            data = self._read_exact(32)
            trigger_id, mono_ns, pre_roll_ns, post_roll_ns = struct.unpack(
                "<QQQQ", data)
            stored = struct.unpack("<I", self._read_exact(4))[0]
            computed = _crc32(type_byte, struct.pack("<Q", trigger_id),
                              struct.pack("<Q", mono_ns),
                              struct.pack("<Q", pre_roll_ns),
                              struct.pack("<Q", post_roll_ns))
            end = self._fp.tell()
            rec = Record(kind="trigger", offset=offset, next_offset=end,
                         record_sequence=trigger_id, mono_ns=mono_ns,
                         crc32=stored, trigger_id=trigger_id,
                         pre_roll_ns=pre_roll_ns, post_roll_ns=post_roll_ns)
        elif kind_byte == RecordType.SESSION:
            data = self._read_exact(12)
            session_id, reason, reserved = struct.unpack("<QHH", data)
            stored = struct.unpack("<I", self._read_exact(4))[0]
            computed = _crc32(type_byte, struct.pack("<Q", session_id),
                              struct.pack("<H", reason))
            end = self._fp.tell()
            rec = Record(kind="session", offset=offset, next_offset=end,
                         record_sequence=session_id, crc32=stored,
                         reason=reason, reserved=reserved,
                         session_id=session_id)
        else:
            raise ThymeFormatError(
                f"unknown record type 0x{kind_byte:02x} at offset {offset}")

        if verify_crc:
            if stored != computed:
                raise ThymeCrcError(f"{rec.kind} record CRC mismatch", offset,
                                    computed, stored)
            rec.crc_ok = True
        else:
            rec.crc_ok = None
        del pad
        return rec

    def iter_records(self, verify_crc: Optional[bool] = None,
                     start: Optional[int] = None,
                     stop: Optional[int] = None) -> Iterator[Record]:
        """Yield records in file order.

        ``start`` / ``stop`` are absolute file offsets. They default to the
        first record and the end of the record stream (footer index start when
        a valid index is present).
        """
        check = self.verify_crc if verify_crc is None else verify_crc
        pos = self.data_offset if start is None else start
        end = self.records_end if stop is None else min(stop, self.records_end)
        if pos < self.data_offset or pos > end:
            raise ValueError("record range is outside the record stream")
        while pos < end:
            if end - pos < 4:
                raise ThymeFormatError(
                    f"truncated record header at offset {pos}")
            rec = self._parse_record(pos, check)
            if rec.next_offset > end:
                raise ThymeFormatError(
                    f"record at offset {pos} crosses the record-stream end")
            pos = rec.next_offset
            yield rec

    def records(self, verify_crc: Optional[bool] = None) -> list[Record]:
        return list(self.iter_records(verify_crc=verify_crc))

    def iter_from(self, mono_ns: int,
                  verify_crc: Optional[bool] = None) -> Iterator[Record]:
        """Seek with the footer index, then stream to the end.

        Without an index the whole stream is scanned (legacy/stream capture);
        this is an explicit API, not a hidden full-file scan on every call.
        """
        start = self.data_offset
        if self.index_status == "ok" and self.index_entries:
            times = [e.mono_ns for e in self.index_entries]
            at = bisect.bisect_right(times, mono_ns)
            if at:
                start = self.index_entries[at - 1].offset
        for rec in self.iter_records(verify_crc=verify_crc, start=start):
            if rec.mono_ns is None or rec.mono_ns >= mono_ns:
                yield rec

    def struct(self, struct_id: int) -> Optional[StructDesc]:
        for desc in self.registry:
            if desc.struct_id == struct_id:
                return desc
        return None

    def summary(self) -> dict:
        """Small machine-readable snapshot (used by ``info --json``)."""
        counts = {"data": 0, "gap": 0, "trigger": 0, "session": 0}
        first_mono: Optional[int] = None
        last_mono: Optional[int] = None
        for rec in self.iter_records():
            counts[rec.kind] += 1
            if rec.mono_ns is not None:
                if first_mono is None or rec.mono_ns < first_mono:
                    first_mono = rec.mono_ns
                if last_mono is None or rec.mono_ns > last_mono:
                    last_mono = rec.mono_ns
        return {
            "path": self.path,
            "file_abi_version": self.header.file_abi_version,
            "device_id": self.header.device_id,
            "session_id": self.header.session_id,
            "start_mono_ns": self.header.start_mono_ns,
            "data_offset": self.data_offset,
            "records_end": self.records_end,
            "structs": [
                {"struct_id": d.struct_id,
                 "name": d.name,
                 "contract_version": d.contract_version,
                 "payload_size": d.payload_size,
                 "layout_hash": (None if d.layout_hash == 0
                                 else f"0x{d.layout_hash:016x}"),
                 "channel": d.channel or None,
                 "record_key": d.record_key or None}
                for d in self.registry
            ],
            "manifest": None if self.manifest is None else {
                "status": self.manifest_status,
                "version": self.manifest.manifest_version,
                "hash": f"0x{self.manifest.manifest_hash:016x}",
            },
            "index": {
                "status": self.index_status,
                "error": self.index_error,
                "blocks": len(self.index_entries),
                "block_size": (self.index_footer.block_size
                               if self.index_footer else None),
                "total_records": (self.index_footer.total_records
                                  if self.index_footer else None),
            },
            "counts": counts,
            "first_mono_ns": first_mono,
            "last_mono_ns": last_mono,
        }
