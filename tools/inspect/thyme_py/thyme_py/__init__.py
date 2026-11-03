"""``thyme_py`` — pure-stdlib reader for thyme capture packages.

Read-only parser for the on-disk ``.thyme.bin`` format (v1-v4, including
the ABI-10 record-manifest block, the ABI-11 keyed registry and the REC-04
footer index) and the ABI-09/ABI-11 ``schema.json`` sidecar. No third-party
dependencies.

Quick start::

    from thyme_py import ThymePackage

    pkg = ThymePackage("/data/capture")
    for item in pkg.iter_records():
        print(item.file, item.record.kind, item.record.mono_ns)

    with pkg.open("small.tbin") as binary:
        print(binary.header.file_abi_version, binary.index_status)
        for record in binary.iter_records():
            ...
"""

from .binary import (
    INDEX_BLOCK_SIZE,
    KEYED_REGISTRY_VERSION,
    KEYED_STRUCT_DESC_BYTES,
    MAX_PAYLOAD_SIZE,
    MAX_STRUCT_COUNT,
    STRUCT_DESC_BYTES,
    SUPPORTED_VERSIONS,
    FileHeader,
    IndexEntry,
    IndexFooter,
    ManifestBlock,
    Record,
    RecordType,
    StructDesc,
    ThymeBinFile,
    ThymeCrcError,
    ThymeError,
    ThymeFormatError,
)
from .package import FileRecord, ThymePackage
from .schema import (
    ChannelDesc,
    FieldDesc,
    PackageSchema,
    RecordDesc,
    ThymeSchemaError,
    decode_record,
    load_schema,
    schema_summary,
)

__all__ = [
    "INDEX_BLOCK_SIZE",
    "KEYED_REGISTRY_VERSION",
    "KEYED_STRUCT_DESC_BYTES",
    "MAX_PAYLOAD_SIZE",
    "MAX_STRUCT_COUNT",
    "STRUCT_DESC_BYTES",
    "SUPPORTED_VERSIONS",
    "ChannelDesc",
    "FileHeader",
    "FileRecord",
    "FieldDesc",
    "IndexEntry",
    "IndexFooter",
    "ManifestBlock",
    "PackageSchema",
    "Record",
    "RecordDesc",
    "RecordType",
    "StructDesc",
    "ThymeBinFile",
    "ThymeCrcError",
    "ThymeError",
    "ThymeFormatError",
    "ThymePackage",
    "ThymeSchemaError",
    "decode_record",
    "load_schema",
    "schema_summary",
]

__version__ = "1.1"
