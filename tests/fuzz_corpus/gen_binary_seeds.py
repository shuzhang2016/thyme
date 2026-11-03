#!/usr/bin/env python3
"""Regenerates the binary fuzz seed corpus for TST-03.

Writes:
  tests/fuzz_corpus/thyme_bin/*.bin        (.thyme.bin reader seeds)
  tests/fuzz_corpus/trigger_protocol/*.bin (trigger datagram seeds)

The byte layouts mirror src/record/thyme_bin.cpp (ThymeBinEncoder) and
include/thyme/record/trigger_protocol.h. CRC32 is zlib.crc32, which matches
the reader's Crc32Update(0xFFFFFFFF, ...) ^ 0xFFFFFFFF convention
(CRC-32/ISO-HDLC, poly 0xEDB88320 reflected).

Run from the repo root:  python3 tests/fuzz_corpus/gen_binary_seeds.py
"""

import os
import struct
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))


def w(path, data):
    full = os.path.join(HERE, path)
    os.makedirs(os.path.dirname(full), exist_ok=True)
    with open(full, "wb") as f:
        f.write(data)
    print("wrote %s (%d bytes)" % (path, len(data)))


# ---------------------------------------------------------------- thyme_bin
HEADER = struct.Struct("<4sHHI4xQQII")        # magic ver rsv dev pad sess mono n rsv
DESC = struct.Struct("<HHI32s")               # struct_id ver payload_size name
DATA_WIRE = struct.Struct("<B3xQHHQII")       # type pad seq sid rsv mono sseq psz
TRIG = struct.Struct("<B3xQQQQ")              # type pad id mono pre post
GAP = struct.Struct("<B3xHHQI")               # type pad sid reason mono missed
SESS = struct.Struct("<B3xQHH")               # type pad new_session reason rsv
FOOTER = struct.Struct("<8sHHIIIQQQQQ")       # magic ver rsv blksz n crc offs...


def data_record(seq, sid, mono, sseq, payload):
    # CRC covers type, seq, struct_id, mono, source_seq, payload_size, payload
    # (NOT the 3 pad bytes and NOT the reserved u16 — see Crc32EntryFields).
    crc_body = struct.pack("<BQHQII", 1, seq, sid, mono, sseq, len(payload))
    crc_body += payload
    crc = zlib.crc32(crc_body)
    return DATA_WIRE.pack(1, seq, sid, 0, mono, sseq, len(payload)) + payload \
        + struct.pack("<I", crc)


def trigger_record(tid, mono, pre, post):
    body = struct.pack("<BQQQQ", 3, tid, mono, pre, post)
    return struct.pack("<B3x", 3) + body[1:] + struct.pack("<I", zlib.crc32(body))


def gap_record(sid, reason, mono, missed):
    body = struct.pack("<BHHQI", 2, sid, reason, mono, missed)
    return struct.pack("<B3x", 2) + body[1:] + struct.pack("<I", zlib.crc32(body))


def session_record(new_session, reason):
    # CRC covers type + new_session + reason; the wire also carries a reserved
    # u16 (see ThymeBinReader kSession path).
    body = struct.pack("<BQH", 4, new_session, reason)
    return SESS.pack(4, new_session, reason, 0) + \
        struct.pack("<I", zlib.crc32(body))


def header(version, descs):
    out = HEADER.pack(b"THYM", version, 0, 0, 0x1234, 1000000, len(descs), 0)
    for d in descs:
        out += DESC.pack(*d)
    return out


DESCS = [(0, 1, 16, b"Demo"), (1, 1, 4, b"Aux")]
PAYLOAD = bytes(range(1, 17))

RECORDS = [
    data_record(1, 0, 1000100, 9, PAYLOAD),
    trigger_record(7, 1000200, 1000, 2000),
    gap_record(1, 1, 1000300, 3),
    session_record(0x9999, 1),
    data_record(2, 1, 1000400, 1, PAYLOAD[:4]),
]


def thyme_bin_seed(with_index):
    out = header(2 if with_index else 1, DESCS)
    data_offset = len(out)
    record_offsets = []
    for rec in RECORDS:
        record_offsets.append(len(out))
        out += rec
    if not with_index:
        return out
    entries_offset = len(out)
    entry = struct.pack(
        "<QQQI", record_offsets[0], 1000100, 1, 0)
    entry += struct.pack("<I", zlib.crc32(entry))
    out += entry
    footer = FOOTER.pack(b"THYIDX1\0", 1, 0, 64, 1, 0,
                         entries_offset, data_offset, len(RECORDS),
                         1000100, 1000400)
    footer = footer[:20] + struct.pack("<I", zlib.crc32(footer)) + footer[24:]
    return out + footer


w("thyme_bin/valid_v2_indexed.bin", thyme_bin_seed(True))
w("thyme_bin/valid_v1_unindexed.bin", thyme_bin_seed(False))
w("thyme_bin/header_only.bin", header(2, DESCS))
w("thyme_bin/empty.bin", b"")
v1 = thyme_bin_seed(False)
w("thyme_bin/truncated_record.bin", v1[:-10])   # cut inside the last record
w("thyme_bin/truncated_header.bin", header(2, DESCS)[:20])

# -------------------------------------------------------- trigger protocol
REQ_V3 = struct.Struct("<IHHi4xQ64sQII")      # 104 bytes (4B pad at offset 12)
REQ_V2 = struct.Struct("<IHHi4xQ64sQ")        # 96 bytes
REQ_V1 = struct.Struct("<IHHi4xQ64s")         # 88 bytes
MAGIC = 0x47495254                            # "TRIG" LE
FLAG_TS = 1
FLAG_CANCEL = 4
FLAG_WAIT = 8
WAIT_ANY = 0xFFFFFFFFFFFFFFFF


def v3(flags, ts, prio, timeout_ms, source, seq):
    body = REQ_V3.pack(MAGIC, 3, flags, prio, ts, source, seq, timeout_ms, 0)
    return body[:100] + struct.pack("<I", zlib.crc32(body[:100]))


w("trigger_protocol/v3_submit.bin", v3(0, 0, -1, 100, b"manual", 1))
w("trigger_protocol/v3_cancel.bin",
  v3(FLAG_TS | FLAG_CANCEL, 1, -1, 100, b"", 1))
w("trigger_protocol/v3_wait.bin",
  v3(FLAG_TS | FLAG_WAIT, WAIT_ANY, -1, 60000, b"", 1))
w("trigger_protocol/v2_submit.bin",
  REQ_V2.pack(MAGIC, 2, 0, -1, 0, b"manual", 1))
w("trigger_protocol/v1_submit.bin", REQ_V1.pack(MAGIC, 1, 0, -1, 0, b"manual"))
w("trigger_protocol/garbage.bin",
  zlib.crc32(b"seed").to_bytes(4, "little") * 25)  # 100 fixed pseudo-bytes
