"""ECO-13 self-describing export decoder (stdlib only).

The file is a 16-byte header plus one self-describing value per record.
Field names travel inside the bytes, so this module does not read schema.json.
"""

from __future__ import annotations

import struct

MAGIC = b"THYSD01\x00"
VERSION = 1
MAX_BYTES = 16 * 1024 * 1024
MAX_DEPTH = 8

_ENCODING = {1: "cbor", 2: "msgpack", 3: "tlv"}


class SelfDescError(ValueError):
    """A document this decoder refuses to guess."""


class _Cur:
    def __init__(self, data: bytes) -> None:
        self.data = data
        self.i = 0

    def need(self, n: int) -> None:
        if n < 0 or self.i + n > len(self.data):
            raise SelfDescError("selfdesc: truncated value")

    def u8(self) -> int:
        self.need(1)
        value = self.data[self.i]
        self.i += 1
        return value

    def take(self, n: int) -> bytes:
        self.need(n)
        chunk = self.data[self.i:self.i + n]
        self.i += n
        return chunk

    def u32le(self) -> int:
        return struct.unpack("<I", self.take(4))[0]

    def u64le(self) -> int:
        return struct.unpack("<Q", self.take(8))[0]

    def u64be(self) -> int:
        return struct.unpack(">Q", self.take(8))[0]


def _depth(depth: int) -> None:
    if depth > MAX_DEPTH:
        raise SelfDescError("selfdesc: nesting exceeds 8")


def _decode_tlv(cur: _Cur, depth: int):
    _depth(depth)
    tag = cur.u8()
    length = cur.u32le()
    if tag < 1 or tag > 9:
        raise SelfDescError("selfdesc: unknown TLV tag")
    if length > MAX_BYTES:
        raise SelfDescError("selfdesc: value exceeds 16MiB")
    end = cur.i + length
    cur.need(length)
    if tag == 1:
        if length != 0:
            raise SelfDescError("selfdesc: null length")
        value = None
    elif tag == 2:
        raw = cur.take(length)
        if length != 1 or raw[0] > 1:
            raise SelfDescError("selfdesc: bad bool")
        value = raw[0] == 1
    elif tag == 3:
        if length != 8:
            raise SelfDescError("selfdesc: bad scalar length")
        value = cur.u64le()
    elif tag == 4:
        if length != 8:
            raise SelfDescError("selfdesc: bad scalar length")
        value = struct.unpack("<q", cur.take(8))[0]
    elif tag == 5:
        if length != 8:
            raise SelfDescError("selfdesc: bad scalar length")
        value = struct.unpack("<d", cur.take(8))[0]
    elif tag == 6:
        value = cur.take(length)
    elif tag == 7:
        value = cur.take(length).decode("utf-8")
    elif tag == 8:
        count = cur.u32le()
        value = [_decode_tlv(cur, depth + 1) for _ in range(count)]
    else:
        count = cur.u32le()
        value = {}
        for _ in range(count):
            key = _decode_tlv(cur, depth + 1)
            if not isinstance(key, str):
                raise SelfDescError("selfdesc: map key is not a string")
            value[key] = _decode_tlv(cur, depth + 1)
    if cur.i != end:
        raise SelfDescError("selfdesc: TLV length does not match value")
    return value


def _cbor_len(cur: _Cur, ai: int) -> int:
    if ai < 24:
        return ai
    if ai == 24:
        return cur.u8()
    if ai == 25:
        return struct.unpack(">H", cur.take(2))[0]
    if ai == 26:
        return struct.unpack(">I", cur.take(4))[0]
    if ai == 27:
        return cur.u64be()
    raise SelfDescError("selfdesc: unknown CBOR additional info")


def _decode_cbor(cur: _Cur, depth: int):
    _depth(depth)
    ib = cur.u8()
    major = ib >> 5
    ai = ib & 0x1F
    if major == 7:
        if ib in (0xF4, 0xF5):
            return ib == 0xF5
        if ib == 0xF6:
            return None
        if ib == 0xFB:
            return struct.unpack(">d", cur.take(8))[0]
        raise SelfDescError("selfdesc: unknown CBOR simple/float tag")
    if major == 6:
        raise SelfDescError("selfdesc: unknown CBOR tag")
    n = _cbor_len(cur, ai)
    if n > MAX_BYTES:
        raise SelfDescError("selfdesc: value exceeds 16MiB")
    if major == 0:
        return n
    if major == 1:
        if n > 2**63 - 1:
            raise SelfDescError("selfdesc: CBOR negative integer out of range")
        return -1 - n
    if major == 2:
        return cur.take(n)
    if major == 3:
        return cur.take(n).decode("utf-8")
    if major == 4:
        return [_decode_cbor(cur, depth + 1) for _ in range(n)]
    if major == 5:
        out = {}
        for _ in range(n):
            key = _decode_cbor(cur, depth + 1)
            if not isinstance(key, str):
                raise SelfDescError("selfdesc: map key is not a string")
            out[key] = _decode_cbor(cur, depth + 1)
        return out
    raise SelfDescError("selfdesc: unknown CBOR major type")


def _mp_be(cur: _Cur, width: int) -> int:
    raw = cur.take(width)
    return int.from_bytes(raw, "big")


def _decode_msgpack(cur: _Cur, depth: int):
    _depth(depth)
    b = cur.u8()
    if b <= 0x7F:
        return b
    if b >= 0xE0:
        return b - 256
    if (b & 0xE0) == 0xA0:
        return cur.take(b & 0x1F).decode("utf-8")
    if (b & 0xF0) == 0x80:
        out = {}
        for _ in range(b & 0x0F):
            key = _decode_msgpack(cur, depth + 1)
            if not isinstance(key, str):
                raise SelfDescError("selfdesc: map key is not a string")
            out[key] = _decode_msgpack(cur, depth + 1)
        return out
    if (b & 0xF0) == 0x90:
        return [_decode_msgpack(cur, depth + 1) for _ in range(b & 0x0F)]
    if b == 0xC0:
        return None
    if b in (0xC2, 0xC3):
        return b == 0xC3
    if b == 0xCA:
        raise SelfDescError("selfdesc: unknown MessagePack float32 tag")
    if b == 0xCB:
        return struct.unpack(">d", cur.take(8))[0]
    widths = {
        0xCC: ("u", 1), 0xCD: ("u", 2), 0xCE: ("u", 4), 0xCF: ("u", 8),
        0xD0: ("i", 1), 0xD1: ("i", 2), 0xD2: ("i", 4), 0xD3: ("i", 8),
        0xC4: ("b", 1), 0xC5: ("b", 2), 0xC6: ("b", 4),
        0xD9: ("s", 1), 0xDA: ("s", 2), 0xDB: ("s", 4),
        0xDC: ("a", 2), 0xDD: ("a", 4),
        0xDE: ("m", 2), 0xDF: ("m", 4),
    }
    spec = widths.get(b)
    if spec is None:
        raise SelfDescError("selfdesc: unknown MessagePack tag")
    kind, width = spec
    n = _mp_be(cur, width)
    if kind == "u":
        return n
    if kind == "i":
        return int.from_bytes(n.to_bytes(width, "big"), "big", signed=True)
    if n > MAX_BYTES:
        raise SelfDescError("selfdesc: value exceeds 16MiB")
    if kind == "b":
        return cur.take(n)
    if kind == "s":
        return cur.take(n).decode("utf-8")
    if kind == "a":
        return [_decode_msgpack(cur, depth + 1) for _ in range(n)]
    out = {}
    for _ in range(n):
        key = _decode_msgpack(cur, depth + 1)
        if not isinstance(key, str):
            raise SelfDescError("selfdesc: map key is not a string")
        out[key] = _decode_msgpack(cur, depth + 1)
    return out


def decode_document(data: bytes):
    """Return ``(encoding_name, records)``. Unknown magic/version/tag raises."""
    if len(data) < 16 or data[:8] != MAGIC:
        raise SelfDescError("selfdesc: bad magic")
    version = data[8]
    if version != VERSION:
        raise SelfDescError("selfdesc: unknown version")
    encoding = _ENCODING.get(data[9])
    if encoding is None or data[10] != 0 or data[11] != 0:
        raise SelfDescError("selfdesc: unknown encoding")
    count = struct.unpack_from("<I", data, 12)[0]
    cur = _Cur(data)
    cur.i = 16
    decode = {"cbor": _decode_cbor, "msgpack": _decode_msgpack,
              "tlv": _decode_tlv}[encoding]
    records = [decode(cur, 1) for _ in range(count)]
    if cur.i != len(data):
        raise SelfDescError("selfdesc: trailing bytes")
    return encoding, records
