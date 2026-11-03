"""CLI: ``python -m thyme_py {info,dump} <pkg-or-file>``."""

from __future__ import annotations

import argparse
import base64
import json
import sys
from typing import Any

from .binary import ThymeError
from .package import FileRecord, ThymePackage


def _record_dict(item: FileRecord, *, hex_payload: bool) -> dict[str, Any]:
    rec = item.record
    out: dict[str, Any] = {
        "file": item.file,
        "offset": rec.offset,
        "kind": rec.kind,
        "mono_ns": rec.mono_ns,
        "record_sequence": rec.record_sequence,
        "crc_ok": rec.crc_ok,
    }
    if rec.kind == "data":
        out["struct_id"] = rec.struct_id
        out["source_sequence"] = rec.source_sequence
        out["payload_size"] = rec.payload_size
        key = "payload_hex" if hex_payload else "payload_b64"
        encoded = (rec.payload.hex() if hex_payload
                   else base64.b64encode(rec.payload).decode("ascii"))
        out[key] = encoded
    elif rec.kind == "gap":
        out["struct_id"] = rec.struct_id
        out["reason"] = rec.reason
        out["missed_count"] = rec.missed_count
    elif rec.kind == "trigger":
        out["trigger_id"] = rec.trigger_id
        out["pre_roll_ns"] = rec.pre_roll_ns
        out["post_roll_ns"] = rec.post_roll_ns
    elif rec.kind == "session":
        out["session_id"] = rec.session_id
        out["reason"] = rec.reason
    return out


def _print_info_text(info: dict[str, Any]) -> None:
    print(f"path   : {info['path']} ({info['kind']})")
    schema = info["schema"]
    if schema["state"] == "absent":
        print("schema : absent")
    else:
        manifest = schema.get("manifest")
        manifest_text = ("none" if manifest is None else
                         f"0x{manifest['hash'][2:]} v{manifest['version']}")
        print(f"schema : ok version={schema['version']} "
              f"partial={str(schema['partial']).lower()} "
              f"records={schema['records']} manifest={manifest_text}")
    for entry in info["files"]:
        print(f"{entry['path']}:")
        print(f"  version      : {entry['file_abi_version']}")
        print(f"  device_id    : 0x{entry['device_id']:x}")
        print(f"  session_id   : 0x{entry['session_id']:x}")
        print(f"  start_mono_ns: {entry['start_mono_ns']}")
        structs = ", ".join(
            f"{s['struct_id']}:{s['name']}/cv{s['contract_version']}"
            f"/{s['payload_size']}B" for s in entry["structs"])
        print(f"  structs      : {structs or '-'}")
        manifest = entry["manifest"]
        if manifest is None:
            print("  manifest     : absent")
        else:
            print(f"  manifest     : {manifest['hash']} "
                  f"version={manifest['version']}")
        counts = entry["counts"]
        print(f"  records      : data={counts['data']} gap={counts['gap']} "
              f"trigger={counts['trigger']} session={counts['session']}")
        index = entry["index"]
        extra = ""
        if index["status"] == "ok":
            extra = (f" blocks={index['blocks']} "
                     f"stride={index['block_size']} "
                     f"total_records={index['total_records']}")
        elif index["error"]:
            extra = f" ({index['error']})"
        print(f"  index        : {index['status']}{extra}")
        print(f"  mono_ns      : {entry['first_mono_ns']} .. "
              f"{entry['last_mono_ns']}")
        print(f"  offsets      : data={entry['data_offset']} "
              f"records_end={entry['records_end']}")


def _cmd_info(args: argparse.Namespace) -> int:
    pkg = ThymePackage(args.path, verify_crc=not args.no_crc)
    info = pkg.info()
    if args.json:
        json.dump(info, sys.stdout, indent=2, sort_keys=True)
        sys.stdout.write("\n")
    else:
        _print_info_text(info)
    return 0


def _cmd_dump(args: argparse.Namespace) -> int:
    pkg = ThymePackage(args.path, verify_crc=not args.no_crc)
    emitted = 0
    for item in pkg.iter_records():
        if args.limit is not None and emitted >= args.limit:
            break
        print(json.dumps(_record_dict(item, hex_payload=args.hex),
                         sort_keys=True))
        emitted += 1
    return 0


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="python -m thyme_py",
        description="Read-only thyme capture package reader (stdlib only).")
    sub = parser.add_subparsers(dest="command", required=True)

    info = sub.add_parser("info", help="package/file summary")
    info.add_argument("path", help="package directory or .thyme.bin file")
    info.add_argument("--json", action="store_true",
                      help="emit machine-readable JSON")
    info.add_argument("--no-crc", action="store_true",
                      help="skip record CRC verification")
    info.set_defaults(func=_cmd_info)

    dump = sub.add_parser("dump", help="stream records as JSON lines")
    dump.add_argument("path", help="package directory or .thyme.bin file")
    dump.add_argument("--limit", type=int, default=None,
                      help="stop after N records")
    dump.add_argument("--hex", action="store_true",
                      help="encode data payloads as hex instead of base64")
    dump.add_argument("--no-crc", action="store_true",
                      help="skip record CRC verification")
    dump.set_defaults(func=_cmd_dump)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _build_parser().parse_args(argv)
    try:
        return args.func(args)
    except (ThymeError, OSError) as exc:
        print(f"thyme_py: error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
