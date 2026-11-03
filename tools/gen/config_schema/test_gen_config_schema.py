#!/usr/bin/env python3
"""Unit tests for gen_config_schema (ARCH-13)."""

from __future__ import annotations

import copy
import io
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parents[2]
sys.path.insert(0, str(SCRIPT_DIR))

import gen_config_schema as gen  # noqa: E402

SCHEMA = gen.load_schema(SCRIPT_DIR / "trigger_config.schema.json")


def _mutated_schema():
    schema = copy.deepcopy(SCHEMA)
    gen.validate(schema)
    return schema


class SchemaValidationTest(unittest.TestCase):
    def test_repository_schema_is_valid(self) -> None:
        gen.validate(SCHEMA)

    def test_duplicate_key_rejected(self) -> None:
        schema = _mutated_schema()
        keys = schema["sections"][0]["keys"]
        keys.append(copy.deepcopy(keys[0]))
        with self.assertRaisesRegex(gen.SchemaError, "duplicate key"):
            gen.validate(schema)

    def test_unknown_status_rejected(self) -> None:
        schema = _mutated_schema()
        schema["sections"][0]["keys"][0]["status"] = "accepted-but-ignored"
        with self.assertRaisesRegex(gen.SchemaError, "status"):
            gen.validate(schema)

    def test_bad_reload_class_rejected(self) -> None:
        schema = _mutated_schema()
        schema["sections"][0]["keys"][0]["reload"] = "someday"
        with self.assertRaisesRegex(gen.SchemaError, "reload"):
            gen.validate(schema)

    def test_rejected_row_requires_error_match(self) -> None:
        schema = _mutated_schema()
        row = next(
            row for row in schema["sections"][0]["keys"]
            if row["status"] == "rejected"
        )
        del row["error_match"]
        with self.assertRaisesRegex(gen.SchemaError, "error_match"):
            gen.validate(schema)

    def test_pipe_in_prose_rejected(self) -> None:
        schema = _mutated_schema()
        schema["sections"][0]["keys"][0]["note"] = "a | b"
        with self.assertRaisesRegex(gen.SchemaError, "must not contain"):
            gen.validate(schema)

    def test_parser_layer_must_be_handwritten(self) -> None:
        schema = _mutated_schema()
        schema["parser_layer"] = "generated"
        with self.assertRaisesRegex(gen.SchemaError, "parser_layer"):
            gen.validate(schema)
        del schema["parser_layer"]
        with self.assertRaisesRegex(gen.SchemaError, "parser_layer"):
            gen.validate(schema)

    def test_gen_graph_channel_keys_are_in_schema(self) -> None:
        channel_keys = {row["key"] for row in gen._section_rows(SCHEMA, "channel.*")}
        self.assertTrue(gen.GEN_GRAPH_CHANNEL_KEYS <= channel_keys)

    def test_golden_ini_unknown_key_fails(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            golden = (root / "tools/gen/gen_graph/testdata/drift/"
                      "generated/trigger.base.ini")
            golden.parent.mkdir(parents=True)
            golden.write_text(
                "[channel./demo_rec]\nkind = mirror_ring\nnot_a_schema_key = 1\n",
                encoding="utf-8")
            with self.assertRaisesRegex(gen.SchemaError, "not_a_schema_key"):
                gen.check_generated_ini_subset(root, SCHEMA)


class EmissionTest(unittest.TestCase):
    def test_emission_is_deterministic(self) -> None:
        self.assertEqual(gen.emit_header(SCHEMA), gen.emit_header(SCHEMA))
        self.assertEqual(gen.emit_audit_block(SCHEMA, "trigger_center"),
                         gen.emit_audit_block(SCHEMA, "trigger_center"))
        self.assertEqual(gen.emit_audit_block(SCHEMA, "recorder"),
                         gen.emit_audit_block(SCHEMA, "recorder"))
        self.assertEqual(gen.emit_reload_block(SCHEMA), gen.emit_reload_block(SCHEMA))

    def test_missing_marker_is_an_error(self) -> None:
        with self.assertRaisesRegex(gen.SchemaError, "missing marker"):
            gen.emit_doc("no markers here\n", SCHEMA)

    def test_repository_artifacts_are_current(self) -> None:
        with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
            self.assertEqual(gen.main(["--check", "--repo-root", str(REPO_ROOT)]), 0)

    def test_write_then_check_round_trip(self) -> None:
        doc = (
            "# doc\n\n"
            f"{gen.AUDIT_BEGIN} generated -->\n"
            f"{gen.AUDIT_END}\n\n"
            f"{gen.AUDIT_MARKERS['recorder'][0]} generated -->\n"
            f"{gen.AUDIT_MARKERS['recorder'][1]}\n\n"
            f"{gen.AUDIT_MARKERS['logging'][0]} generated -->\n"
            f"{gen.AUDIT_MARKERS['logging'][1]}\n\n"
            f"{gen.AUDIT_MARKERS['security'][0]} generated -->\n"
            f"{gen.AUDIT_MARKERS['security'][1]}\n\n"
            f"{gen.AUDIT_MARKERS['continuous'][0]} generated -->\n"
            f"{gen.AUDIT_MARKERS['continuous'][1]}\n\n"
            f"{gen.AUDIT_MARKERS['chassis'][0]} generated -->\n"
            f"{gen.AUDIT_MARKERS['chassis'][1]}\n\n"
            f"{gen.AUDIT_MARKERS['quota.*'][0]} generated -->\n"
            f"{gen.AUDIT_MARKERS['quota.*'][1]}\n\n"
            f"{gen.AUDIT_MARKERS['source.*'][0]} generated -->\n"
            f"{gen.AUDIT_MARKERS['source.*'][1]}\n\n"
            f"{gen.AUDIT_MARKERS['channel.*'][0]} generated -->\n"
            f"{gen.AUDIT_MARKERS['channel.*'][1]}\n\n"
            f"{gen.RELOAD_BEGIN} generated -->\n"
            f"{gen.RELOAD_END}\n"
        )
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / gen.DOC_REL).parent.mkdir(parents=True)
            (root / gen.DOC_REL).write_text(doc, encoding="utf-8")
            with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
                self.assertEqual(gen.main(["--repo-root", str(root)]), 0)
                self.assertEqual(
                    gen.main(["--check", "--repo-root", str(root)]), 0)
                header = root / gen.HEADER_REL
                header.write_text(header.read_text(encoding="utf-8") + "// stale\n",
                                  encoding="utf-8")
                self.assertEqual(
                    gen.main(["--check", "--repo-root", str(root)]),
                    gen.EXIT_ERROR)


if __name__ == "__main__":
    unittest.main()
