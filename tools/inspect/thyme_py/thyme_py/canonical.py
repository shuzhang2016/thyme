"""REP-15 protobuf and CDR decoders for thyme's export subset.

The caller supplies the field table (from the generated .proto/.msg or
schema.json). These bytes do not carry field names.
"""

from __future__ import annotations

import struct


class CanonicalError(ValueError):
    """A payload this decoder refuses to guess."""


def _varint(data: bytes, i: int) -> tuple[int, int]:
    value = 0
    shift = 0
    for _ in range(10):
        if i >= len(data):
            raise CanonicalError("canonical: truncated value")
        byte = data[i]
        i += 1
        value |= (byte & 0x7F) << shift
        if (byte & 0x80) == 0:
            return value, i
        shift += 7
    raise CanonicalError("canonical: bad protobuf varint")


def decode_protobuf(data: bytes, fields: list[tuple[str, str]]) -> dict:
    """fields is (name, type) in field-number order. type is uint32/int32/string."""
    found: dict[int, tuple[int, bytes | int]] = {}
    i = 0
    while i < len(data):
        key, i = _varint(data, i)
        number = key >> 3
        wire = key & 7
        if number == 0 or number > len(fields):
            raise CanonicalError("canonical: unknown protobuf field")
        if wire == 0:
            value, i = _varint(data, i)
            found[number] = (wire, value)
        elif wire == 2:
            length, i = _varint(data, i)
            if i + length > len(data):
                raise CanonicalError("canonical: truncated value")
            found[number] = (wire, data[i:i + length])
            i += length
        else:
            raise CanonicalError("canonical: unknown protobuf wire type")
    out = {}
    for index, (name, typ) in enumerate(fields, start=1):
        if index not in found:
            raise CanonicalError(f"canonical: missing protobuf field '{name}'")
        wire, raw = found[index]
        if typ == "uint32":
            if wire != 0:
                raise CanonicalError("canonical: bad protobuf integer")
            out[name] = raw
        elif typ == "int32":
            if wire != 0:
                raise CanonicalError("canonical: bad protobuf integer")
            out[name] = struct.unpack("<i", struct.pack("<I", raw & 0xFFFFFFFF))[0]
        elif typ == "string":
            if wire != 2 or not isinstance(raw, bytes):
                raise CanonicalError("canonical: bad protobuf string")
            out[name] = raw.decode("utf-8")
        else:
            raise CanonicalError("canonical: unknown field type")
    return out


def decode_cdr_u32(data: bytes) -> int:
    """Little-endian CDR encapsulation plus one uint32."""
    if len(data) != 8 or data[:4] != b"\x00\x01\x00\x00":
        raise CanonicalError("canonical: unknown CDR encapsulation")
    return struct.unpack_from("<I", data, 4)[0]
