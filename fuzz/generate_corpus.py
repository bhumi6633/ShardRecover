#!/usr/bin/env python3
"""Regenerate ShardRecover's small deterministic libFuzzer seed corpora."""

from pathlib import Path
import struct
import zlib


ROOT = Path(__file__).resolve().parent / "corpus"


def chunk(kind: bytes, data: bytes = b"", valid_crc: bool = True) -> bytes:
    checksum = zlib.crc32(kind + data)
    if not valid_crc:
        checksum ^= 1
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", checksum)


signature = b"\x89PNG\r\n\x1a\n"
ihdr_data = struct.pack(">IIBBBBB", 1, 1, 8, 6, 0, 0, 0)
ihdr = chunk(b"IHDR", ihdr_data)
idat = chunk(b"IDAT")
iend = chunk(b"IEND")

png_seeds = {
    "empty": b"",
    "partial-signature": signature[:4],
    "signature-only": signature,
    "minimal-valid.png": signature + ihdr + idat + iend,
    "valid-ihdr": signature + ihdr,
    "malicious-length": signature + b"\xff\xff\xff\xffIDAT",
    "truncated-chunk": signature + struct.pack(">I", 5) + b"IDATxx",
    "bad-crc": signature + ihdr + chunk(b"IDAT", valid_crc=False) + iend,
    "duplicate-ihdr": signature + ihdr + ihdr + idat + iend,
    "missing-iend": signature + ihdr + idat,
    "trailing-bytes": signature + ihdr + idat + iend + b"trailing",
}

# First three bytes encode split, minimum overlap, and mismatch budget.
overlap_seeds = {
    "exact-overlap": bytes([128, 1, 0]) + b"ABCBCD",
    "no-overlap": bytes([128, 0, 0]) + b"ABCXYZ",
    "one-mismatch": bytes([128, 1, 1]) + b"ABCDCxEF",
    "multiple-mismatches": bytes([128, 1, 2]) + b"ABCDxyEF",
    "empty-left": bytes([0, 0, 0]) + b"payload",
    "empty-right": bytes([255, 0, 0]) + b"payload",
    "binary-bytes": bytes([128, 0, 1]) + b"\x00\xff\x80\x13\x80\x13\x7a\x00",
    "directional": bytes([128, 1, 0]) + b"ABCDEFCDEFZZ",
    "minimum-boundary": bytes([128, 255, 0]) + b"AAAAAA",
}

for corpus_name, seeds in (("png", png_seeds), ("overlap", overlap_seeds)):
    directory = ROOT / corpus_name
    directory.mkdir(parents=True, exist_ok=True)
    for existing in directory.iterdir():
        if existing.is_file() and existing.name not in seeds:
            existing.unlink()
    for name, contents in seeds.items():
        (directory / name).write_bytes(contents)
