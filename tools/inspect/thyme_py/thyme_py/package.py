"""Package-level facade over ``.thyme.bin`` files and ``schema.json``."""

from __future__ import annotations

import os
from dataclasses import dataclass
from typing import Iterator, Optional

from .binary import Record, ThymeBinFile, ThymeError
from .schema import PackageSchema, decode_record, load_schema, schema_summary


@dataclass(frozen=True)
class FileRecord:
    """A record plus the package-relative file it came from."""

    file: str
    record: Record


class ThymePackage:
    """Read-only view of a capture package directory (or a single file).

    A directory contributes every regular ``*.tbin`` below it (sorted,
    recursive) plus the ``schema.json`` sidecar at its root. A file path
    contributes just that file and, for a sidecar, its sibling directory.
    """

    def __init__(self, path: os.PathLike[str] | str,
                 verify_crc: bool = True):
        self.path = os.path.abspath(os.fspath(path))
        self.verify_crc = verify_crc
        if os.path.isdir(self.path):
            self.kind = "package"
            self.root = self.path
            self.files = sorted(
                os.path.relpath(os.path.join(base, name), self.path)
                for base, _dirs, names in os.walk(self.path)
                for name in names
                if name.endswith(".tbin")
            )
        elif os.path.isfile(self.path):
            self.kind = "file"
            self.root = os.path.dirname(self.path)
            self.files = [os.path.basename(self.path)]
        else:
            raise ThymeError(f"no such package or file: {self.path}")

        self.schema: Optional[PackageSchema] = load_schema(self.root)
        if self.schema is None:
            self.schema_state = "absent"
        else:
            self.schema_state = "ok"

    def __repr__(self) -> str:
        return (f"ThymePackage({self.path!r}, kind={self.kind!r}, "
                f"files={len(self.files)})")

    def open(self, relative_file: str,
             verify_crc: Optional[bool] = None) -> ThymeBinFile:
        return ThymeBinFile(os.path.join(self.root, relative_file),
                            verify_crc=(self.verify_crc
                                        if verify_crc is None else verify_crc))

    def iter_records(self, verify_crc: Optional[bool] = None
                     ) -> Iterator[FileRecord]:
        """Yield every record in every file, file name order."""
        for relative_file in self.files:
            with self.open(relative_file, verify_crc=verify_crc) as binary:
                for record in binary.iter_records():
                    yield FileRecord(relative_file, record)

    def records(self, verify_crc: Optional[bool] = None) -> list[FileRecord]:
        return list(self.iter_records(verify_crc=verify_crc))

    def decode(self, file_record: FileRecord) -> dict:
        """Decode known fields of a data record through the sidecar."""
        if self.schema is None:
            raise ThymeError("package has no schema.json sidecar")
        return decode_record(self.schema, file_record.record)

    def info(self) -> dict:
        """Machine-readable summary used by ``python -m thyme_py info``."""
        files = []
        for relative_file in self.files:
            with self.open(relative_file) as binary:
                entry = binary.summary()
            entry["path"] = relative_file
            files.append(entry)
        return {
            "format": "thyme-python-info",
            "version": 1,
            "path": self.path,
            "kind": self.kind,
            "schema": schema_summary(self.schema),
            "files": files,
        }
