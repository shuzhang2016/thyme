"""ECO-13: stdlib decoder of the self-describing export.

The TLV document is the same byte string the C++ test decodes. A consumer
does not need schema.json, a Foxglove plugin, or the C++ fixture.
"""

from __future__ import annotations

import unittest

from thyme_py.selfdesc import SelfDescError, decode_document

_GOLDEN = bytes([
    0x54, 0x48, 0x59, 0x53, 0x44, 0x30, 0x31, 0x00,
    0x01, 0x03, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x09, 0x17, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00,
    0x07, 0x01, 0x00, 0x00, 0x00, ord("n"),
    0x03, 0x08, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
])


def _header(encoding: int, body: bytes) -> bytes:
    return (b"THYSD01\x00" + bytes([1, encoding, 0, 0])
            + (1).to_bytes(4, "little") + body)


class SelfDescTest(unittest.TestCase):
    def test_golden_tlv_map(self) -> None:
        encoding, records = decode_document(_GOLDEN)
        self.assertEqual(encoding, "tlv")
        self.assertEqual(records, [{"n": 1}])

    def test_cbor_null_and_msgpack_nil(self) -> None:
        enc, recs = decode_document(_header(1, b"\xf6"))
        self.assertEqual((enc, recs), ("cbor", [None]))
        enc, recs = decode_document(_header(2, b"\xc0"))
        self.assertEqual((enc, recs), ("msgpack", [None]))

    def test_unknown_version_and_tags(self) -> None:
        bad_ver = bytearray(_GOLDEN)
        bad_ver[8] = 2
        with self.assertRaises(SelfDescError) as ctx:
            decode_document(bytes(bad_ver))
        self.assertIn("unknown version", str(ctx.exception))
        with self.assertRaises(SelfDescError) as ctx:
            decode_document(_header(3, b"\xfe\x00\x00\x00\x00"))
        self.assertIn("unknown TLV tag", str(ctx.exception))
        with self.assertRaises(SelfDescError) as ctx:
            decode_document(_header(1, b"\xc0"))
        self.assertIn("unknown CBOR tag", str(ctx.exception))
        with self.assertRaises(SelfDescError) as ctx:
            decode_document(_header(2, b"\xca"))
        self.assertIn("float32", str(ctx.exception))


if __name__ == "__main__":
    unittest.main()
