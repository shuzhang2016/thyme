#!/usr/bin/env python3

import argparse
import difflib
import json
from pathlib import Path
import subprocess
import sys
import tempfile

SKIP_RETURN_CODE = 77
# Set by --require-deps (the CI reference-parser job): a missing dependency is
# then a hard failure instead of a visible skip, so the job cannot go green
# without actually running the check.
REQUIRE_DEPS = False


def run(command):
    result = subprocess.run(command, text=True, capture_output=True, check=False)
    if result.returncode != 0:
        raise RuntimeError(
            f"command failed ({result.returncode}): {' '.join(map(str, command))}\n"
            f"{result.stdout}{result.stderr}"
        )


def official_summary(mcap_path):
    from mcap.reader import make_reader

    with mcap_path.open("rb") as stream:
        reader = make_reader(stream, validate_crcs=True)
        header = reader.get_header()
        summary = reader.get_summary()
        if summary is None:
            raise RuntimeError("official mcap reader found no summary")
        schemas = sorted(summary.schemas.values(), key=lambda item: item.id)
        channels = sorted(summary.channels.values(), key=lambda item: item.id)
        topics = {channel.id: channel.topic for channel in channels}
        messages = []
        topic_message_counts = {channel.topic: 0 for channel in channels}
        for _, channel, message in reader.iter_messages():
            messages.append(
                {
                    "channel_id": message.channel_id,
                    "sequence": message.sequence,
                    "log_time": message.log_time,
                    "publish_time": message.publish_time,
                }
            )
            topic_message_counts[topics[channel.id]] += 1
        stats = summary.statistics
        return {
            "header": {"profile": header.profile, "library": header.library},
            "schemas": [
                {
                    "id": schema.id,
                    "name": schema.name,
                    "encoding": schema.encoding,
                    "data_hex": schema.data.hex(),
                }
                for schema in schemas
            ],
            "channels": [
                {
                    "id": channel.id,
                    "schema_id": channel.schema_id,
                    "topic": channel.topic,
                    "message_encoding": channel.message_encoding,
                }
                for channel in channels
            ],
            "messages": messages,
            "message_count": len(messages),
            "topic_message_counts": topic_message_counts,
            "statistics": {
                "message_count": stats.message_count,
                "schema_count": stats.schema_count,
                "channel_count": stats.channel_count,
                "attachment_count": stats.attachment_count,
                "metadata_count": stats.metadata_count,
                "chunk_count": stats.chunk_count,
                "start_time": stats.message_start_time,
                "end_time": stats.message_end_time,
                "channel_message_counts": {
                    str(channel_id): count
                    for channel_id, count in stats.channel_message_counts.items()
                },
            },
        }


def compare(expected, actual):
    if expected == actual:
        return
    expected_lines = json.dumps(expected, indent=2, sort_keys=True).splitlines()
    actual_lines = json.dumps(actual, indent=2, sort_keys=True).splitlines()
    diff = "\n".join(
        difflib.unified_diff(expected_lines, actual_lines, fromfile="thyme-self-check", tofile="official-mcap")
    )
    raise RuntimeError(f"MCAP reference mismatch:\n{diff}")


def main():
    global REQUIRE_DEPS
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture", required=True, type=Path)
    parser.add_argument("--replay", required=True, type=Path)
    parser.add_argument("--require-deps", action="store_true",
                        help="fail instead of skipping when mcap/PyYAML is missing")
    args = parser.parse_args()
    REQUIRE_DEPS = args.require_deps

    try:
        import mcap  # noqa: F401
    except ImportError as error:
        if REQUIRE_DEPS:
            print(f"FAIL: official Python mcap library is required: {error}",
                  file=sys.stderr)
            return 1
        print(f"SKIP: official Python mcap library is unavailable: {error}")
        return SKIP_RETURN_CODE

    with tempfile.TemporaryDirectory(prefix="thyme_eco12_mcap_") as temporary:
        root = Path(temporary) / "case"
        package = root / "pkg"
        mcap_path = root / "reference.mcap"
        schema_path = root / "schema.json"
        expected_path = root / "self-check.json"
        run([args.fixture, "prepare", root])
        run(
            [
                args.replay,
                "export",
                package,
                "--out",
                mcap_path,
                "--format",
                "mcap",
                "--schema-out",
                schema_path,
            ]
        )
        run([args.fixture, "dump", mcap_path, expected_path])
        expected = json.loads(expected_path.read_text(encoding="utf-8"))
        actual = official_summary(mcap_path)
        compare(expected, actual)
        print(
            "ECO-12 MCAP official Python cross-check OK: "
            f"{actual['message_count']} messages, {len(actual['schemas'])} schemas, "
            f"{len(actual['channels'])} topics; topic/schema/timestamp/count differences: 0"
        )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except ImportError as error:
        if REQUIRE_DEPS:
            print(f"FAIL: official Python dependency is required: {error}",
                  file=sys.stderr)
            sys.exit(1)
        print(f"SKIP: official Python mcap dependency is unavailable: {error}")
        sys.exit(SKIP_RETURN_CODE)
    except (OSError, RuntimeError, ValueError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        sys.exit(1)
