"""REP-15: Python decodes protobuf/CDR using a field table, not the bytes' names."""

from __future__ import annotations

import unittest

from thyme_py.canonical import CanonicalError, decode_cdr_u32, decode_protobuf


class CanonicalTest(unittest.TestCase):
    def test_protobuf_and_cdr_scalars(self) -> None:
        self.assertEqual(decode_protobuf(b"\x08\x2a", [("n", "uint32")]),
                         {"n": 42})
        minus = b"\x08" + b"\xff" * 9 + b"\x01"
        self.assertEqual(decode_protobuf(minus, [("n", "int32")]), {"n": -1})
        self.assertEqual(decode_cdr_u32(b"\x00\x01\x00\x00\x2a\x00\x00\x00"), 42)

    def test_unknown_field_and_encapsulation(self) -> None:
        with self.assertRaises(CanonicalError) as ctx:
            decode_protobuf(b"\x10\x01", [("n", "uint32")])
        self.assertIn("unknown protobuf field", str(ctx.exception))
        with self.assertRaises(CanonicalError) as ctx:
            decode_cdr_u32(b"\x00\x00\x00\x00\x2a\x00\x00\x00")
        self.assertIn("unknown CDR", str(ctx.exception))


if __name__ == "__main__":
    unittest.main()
