#!/usr/bin/env python3
"""REP-15 cross-check: parse the exported FileDescriptorSet with the official
protobuf library.

The exporter writes the descriptor set with a hand-rolled encoder
(`canonical::RenderFileDescriptorSet`). MCAP stores it as opaque bytes, so the
official mcap reader cannot prove it is valid. This test does: a real
`google.protobuf` parser must accept it and see the same message/field shape.

Missing `protobuf` is a visible CTest skip (77), never a silent pass and never
a runtime dependency.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

SKIP_RETURN_CODE = 77
# Set by --require-deps (CI reference-parser job): missing protobuf fails
# instead of skipping, so the job cannot pass without parsing the descriptor.
REQUIRE_DEPS = False


def main() -> int:
    global REQUIRE_DEPS
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--fixture", required=True,
        help="descriptor_fixture executable that writes the descriptor set")
    parser.add_argument("--require-deps", action="store_true",
                        help="fail instead of skipping when protobuf is missing")
    args = parser.parse_args()
    REQUIRE_DEPS = args.require_deps

    try:
        from google.protobuf import descriptor_pb2
    except ImportError:
        if REQUIRE_DEPS:
            print("FAIL: protobuf is required (tests/eco12-requirements.txt)",
                  file=sys.stderr)
            return 1
        print("skip: protobuf not installed (tests/eco12-requirements.txt)",
              file=sys.stderr)
        return SKIP_RETURN_CODE

    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / "descriptor_set.bin"
        subprocess.run([args.fixture, str(out)], check=True)
        data = out.read_bytes()
    fds = descriptor_pb2.FileDescriptorSet()
    fds.ParseFromString(data)

    if len(fds.file) != 1:
        raise AssertionError(f"expected one FileDescriptorProto, got {len(fds.file)}")
    proto = fds.file[0]
    assert proto.name == "thyme.proto", proto.name
    assert proto.package == "thyme", proto.package
    assert proto.syntax == "proto3", proto.syntax

    messages = {m.name: m for m in proto.message_type}
    assert set(messages) == {"Outer", "Pt"}, sorted(messages)

    outer = messages["Outer"]
    fields = {f.name: f for f in outer.field}
    assert set(fields) == {"n", "name", "xs", "ratio", "pt", "opt"}, sorted(fields)

    fd = descriptor_pb2.FieldDescriptorProto
    assert fields["n"].number == 1
    assert fields["n"].type == fd.TYPE_UINT32
    assert fields["n"].label == fd.LABEL_OPTIONAL
    assert fields["xs"].label == fd.LABEL_REPEATED
    assert fields["pt"].type == fd.TYPE_MESSAGE
    assert fields["pt"].type_name == ".thyme.Pt", fields["pt"].type_name
    if not hasattr(fields["opt"], "proto3_optional"):
        print("SKIP: installed protobuf does not expose proto3_optional")
        return 0
    assert fields["opt"].proto3_optional is True
    assert fields["opt"].label == fd.LABEL_OPTIONAL

    inner = messages["Pt"]
    assert {f.name for f in inner.field} == {"x", "y"}
    print("rep15 descriptor set parses with the official protobuf library")
    return 0


if __name__ == "__main__":
    sys.exit(main())
