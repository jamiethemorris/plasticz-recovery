#!/usr/bin/env python3
"""Lossless codec for the recovered PlastiCZ raw host state chunk.

Supported subset: 1..128 programs, exactly 38 normalized finite float32 values
per program, and a current-program index inside the supplied bank. Layout:
three uint32 header words (program count, parameter count, current program),
all programs' float words in program order, then optionally all 32-byte names.
Header/float words may be little- or big-endian; names are never byte-swapped.

Static evidence: analysis/evidence/state.rea.json, getChunk 0x13204
(ev_172c82dd8a5a306d07c733776edd0470494349f057b918054d8865ec985e1c42)
and setChunk 0x1356e
(ev_7d4b9ef0dc78c2b02e57205b9135c303eb6fb70f5a6e1e8f35524f5058196aa0).
setChunk's endian discriminator tests the first LE word & 0xffff0000. The
recovered getter always emits names; the setter also accepts the nameless form.
This codec deliberately rejects unsupported counts, invalid values, and extra
bytes rather than reproducing the original setter's permissive parsing.

This is NOT an FXP/FXB, AU preset, or DAW project-file container codec. No host
restore or audio equivalence is established by these static layout findings.

Examples:
  python3 scripts/plasticz_state.py --decode old.chunk decoded.json
  python3 scripts/plasticz_state.py --encode decoded.json restored.chunk
  python3 scripts/plasticz_state.py --from-factory analysis/recovered/factory-presets.json factory.chunk
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import struct
from typing import Any


FORMAT = "plasticz-raw-state-chunk"
PARAMETERS = 38
MAX_PROGRAMS = 128
NAME_BYTES = 32
HEADER_BYTES = 12
MAX_CHUNK_BYTES = HEADER_BYTES + MAX_PROGRAMS * (PARAMETERS * 4 + NAME_BYTES)
BYTE_ORDERS = {"little": "<", "big": ">"}
HEX_WORD = re.compile(r"[0-9a-fA-F]{8}\Z")
HEX_NAME = re.compile(r"[0-9a-fA-F]{64}\Z")


class StateError(ValueError):
    """The input is malformed or outside the explicitly supported subset."""


def require(condition: bool, message: str) -> None:
    if not condition:
        raise StateError(message)


def _header(program_count: Any, parameter_count: Any, current_program: Any) -> None:
    require(type(program_count) is int and 1 <= program_count <= MAX_PROGRAMS,
            "program_count must be an integer in 1..128")
    require(type(parameter_count) is int and parameter_count == PARAMETERS,
            "Only parameter_count=38 is supported")
    require(type(current_program) is int and 0 <= current_program < program_count,
            "current_program must index a program in the supplied bank")


def _prefix(byte_order: Any) -> str:
    require(isinstance(byte_order, str) and byte_order in BYTE_ORDERS,
            "byte_order must be little or big")
    return BYTE_ORDERS[byte_order]


def _value(word: int, context: str) -> float:
    value = struct.unpack("<f", struct.pack("<I", word))[0]
    require(math.isfinite(value) and 0.0 <= value <= 1.0,
            f"{context}: float32 value must be finite and within [0,1]")
    return value


def _words(program: dict, index: int) -> list[int]:
    """Hex words are lossless; redundant decimal values must agree at float32 precision."""
    hex_values = program.get("float32_bits_hex")
    values = program.get("values")
    require(hex_values is not None or values is not None,
            f"Program {index}: float32_bits_hex or values is required")
    words = None
    if hex_values is not None:
        require(isinstance(hex_values, list) and len(hex_values) == PARAMETERS,
                f"Program {index}: exactly 38 float32_bits_hex entries required")
        require(all(isinstance(h, str) and HEX_WORD.fullmatch(h) for h in hex_values),
                f"Program {index}: float32 words must each contain 8 hex digits")
        words = [int(h, 16) for h in hex_values]
        for parameter, word in enumerate(words):
            _value(word, f"Program {index}, parameter {parameter}")
    if values is not None:
        require(isinstance(values, list) and len(values) == PARAMETERS,
                f"Program {index}: exactly 38 values required")
        from_values = []
        for parameter, value in enumerate(values):
            require(type(value) in (int, float) and 0 <= value <= 1 and math.isfinite(value),
                    f"Program {index}, parameter {parameter}: value must be finite and within [0,1]")
            from_values.append(struct.unpack("<I", struct.pack("<f", value))[0])
        require(words is None or words == from_values,
                f"Program {index}: values and float32_bits_hex disagree; update both or omit one")
        words = from_values
    return words


def decode_chunk(data: bytes) -> dict:
    """Decode exactly one raw chunk; preserve all float words and name-storage bytes."""
    require(isinstance(data, (bytes, bytearray, memoryview)), "Chunk must be bytes")
    data = bytes(data)
    require(HEADER_BYTES <= len(data) <= MAX_CHUNK_BYTES,
            f"Chunk size must be between {HEADER_BYTES} and {MAX_CHUNK_BYTES} bytes")
    if data[:4] == b"CcnK":
        raise StateError("FXP/FXB container detected; this codec accepts only the extracted raw host chunk")
    first_le = struct.unpack_from("<I", data)[0]
    byte_order = "big" if first_le & 0xFFFF0000 else "little"
    prefix = _prefix(byte_order)
    program_count, parameter_count, current_program = struct.unpack_from(prefix + "3I", data)
    _header(program_count, parameter_count, current_program)
    values_end = HEADER_BYTES + program_count * parameter_count * 4
    with_names_size = values_end + program_count * NAME_BYTES
    require(len(data) in (values_end, with_names_size),
            f"Exact chunk size must be {values_end} without names or {with_names_size} with names; got {len(data)}")
    names_present = len(data) == with_names_size
    programs = []
    for index in range(program_count):
        offset = HEADER_BYTES + index * parameter_count * 4
        words = struct.unpack_from(prefix + "38I", data, offset)
        program = {
            "index": index,
            "float32_bits_hex": [f"{word:08x}" for word in words],
            "values": [_value(word, f"Program {index}, parameter {parameter}")
                       for parameter, word in enumerate(words)],
        }
        if names_present:
            name = data[values_end + index * NAME_BYTES:values_end + (index + 1) * NAME_BYTES]
            program["name_storage_hex"] = name.hex()
            # Latin-1 is a reversible byte view, not a claim about the old UI encoding.
            program["name_display_latin1"] = name.split(b"\0", 1)[0].decode("latin-1")
        programs.append(program)
    return {
        "format": FORMAT,
        "schema_version": 1,
        "byte_order": byte_order,
        "program_count": program_count,
        "parameter_count": parameter_count,
        "current_program": current_program,
        "names_present": names_present,
        "source_chunk_sha256": hashlib.sha256(data).hexdigest(),
        "name_display_note": "Latin-1 is a byte-preserving view; original text encoding is unknown. name_storage_hex is authoritative.",
        "programs": programs,
    }


def encode_chunk(document: dict, *, byte_order: str | None = None,
                 include_names: bool | None = None, current_program: int | None = None) -> bytes:
    """Encode codec JSON, preserving source endianness unless explicitly overridden.

    Names must already exist when include_names=True; this never invents names.
    Decimal-only values are rounded to float32. Raw words preserve negative zero
    and subnormal values exactly. source_chunk_sha256 and display text are
    informational, and never replace the authoritative binary field data.
    """
    require(isinstance(document, dict), "JSON root must be an object")
    require(document.get("format") == FORMAT and type(document.get("schema_version")) is int
            and document["schema_version"] == 1, "Expected plasticz-raw-state-chunk JSON schema version 1")
    program_count = document.get("program_count")
    parameter_count = document.get("parameter_count")
    current = document.get("current_program") if current_program is None else current_program
    _header(program_count, parameter_count, current)
    prefix = _prefix(document.get("byte_order") if byte_order is None else byte_order)
    names_present = document.get("names_present")
    require(type(names_present) is bool, "names_present must be a boolean")
    require(include_names is None or type(include_names) is bool, "include_names must be a boolean")
    use_names = names_present if include_names is None else include_names
    require(not use_names or names_present, "Cannot include names absent from the input")
    programs = document.get("programs")
    require(isinstance(programs, list) and len(programs) == program_count,
            "programs length must equal program_count")
    values = bytearray()
    names = bytearray()
    for index, program in enumerate(programs):
        require(isinstance(program, dict) and type(program.get("index")) is int
                and program["index"] == index, "Program indices must be contiguous and in array order")
        words = _words(program, index)
        values += struct.pack(prefix + "38I", *words)
        hex_name = program.get("name_storage_hex")
        if names_present:
            require(isinstance(hex_name, str) and HEX_NAME.fullmatch(hex_name),
                    f"Program {index}: name_storage_hex must encode exactly 32 bytes")
            names += bytes.fromhex(hex_name)
        else:
            require(hex_name is None, f"Program {index}: name data conflicts with names_present=false")
    return struct.pack(prefix + "3I", program_count, parameter_count, current) + values + (names if use_names else b"")


def from_factory(factory: dict, *, byte_order: str = "little", include_names: bool = True,
                 current_program: int = 0) -> dict:
    """Convert extract_factory.py's JSON to codec JSON (no FXP/FXB wrapper)."""
    require(isinstance(factory, dict), "Factory JSON root must be an object")
    _header(factory.get("program_count"), factory.get("parameter_count"), current_program)
    require(type(include_names) is bool, "include_names must be a boolean")
    source_programs = factory.get("programs")
    require(isinstance(source_programs, list) and len(source_programs) == factory["program_count"],
            "Factory programs length must equal program_count")
    programs = []
    for index, source in enumerate(source_programs):
        require(isinstance(source, dict) and type(source.get("index")) is int and source["index"] == index,
                "Factory program indices must be contiguous and in array order")
        words = _words(source, index)
        program = {"index": index, "float32_bits_hex": [f"{word:08x}" for word in words]}
        if include_names:
            program["name_storage_hex"] = source.get("name_storage_hex")
        programs.append(program)
    document = dict(format=FORMAT, schema_version=1, byte_order=byte_order,
                    program_count=factory["program_count"], parameter_count=PARAMETERS,
                    current_program=current_program, names_present=include_names, programs=programs)
    return decode_chunk(encode_chunk(document))


def _unique_object(pairs: list[tuple[str, Any]]) -> dict:
    result = {}
    for key, value in pairs:
        require(key not in result, f"Duplicate JSON key: {key}")
        result[key] = value
    return result


def _reject_constant(value: str) -> None:
    raise StateError(f"Non-finite JSON constant: {value}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    modes = parser.add_mutually_exclusive_group(required=True)
    modes.add_argument("--decode", nargs=2, metavar=("CHUNK", "JSON"))
    modes.add_argument("--encode", nargs=2, metavar=("JSON", "CHUNK"))
    modes.add_argument("--from-factory", nargs=2, metavar=("FACTORY_JSON", "CHUNK"))
    parser.add_argument("--byte-order", choices=BYTE_ORDERS, help="Explicit endian conversion when encoding")
    parser.add_argument("--without-names", action="store_true", help="Explicitly omit names when encoding")
    parser.add_argument("--current-program", type=int, help="Explicitly select current program when encoding")
    args = parser.parse_args()
    source_name, output_name = args.decode or args.encode or args.from_factory
    source, output = Path(source_name), Path(output_name)
    try:
        require(source.resolve() != output.resolve(), "Input and output paths must differ")
        if args.decode:
            require(args.byte_order is None and not args.without_names and args.current_program is None,
                    "Encoding options cannot be used with --decode")
            with source.open("rb") as stream:
                document = decode_chunk(stream.read(MAX_CHUNK_BYTES + 1))
            payload = (json.dumps(document, indent=2, ensure_ascii=True, allow_nan=False) + "\n").encode("utf-8")
        else:
            document = json.loads(source.read_text(encoding="utf-8"), object_pairs_hook=_unique_object,
                                  parse_constant=_reject_constant)
            if args.from_factory:
                document = from_factory(document, byte_order=args.byte_order or "little",
                                        include_names=not args.without_names,
                                        current_program=0 if args.current_program is None else args.current_program)
                payload = encode_chunk(document)
            else:
                payload = encode_chunk(document, byte_order=args.byte_order,
                                       include_names=False if args.without_names else None,
                                       current_program=args.current_program)
        output.write_bytes(payload)
    except (OSError, ValueError, OverflowError) as error:
        parser.exit(2, f"plasticz_state: {error}\n")
    print(f"Wrote {output} ({len(payload)} bytes)")


if __name__ == "__main__":
    main()
