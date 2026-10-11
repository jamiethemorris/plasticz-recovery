#!/usr/bin/env python3
"""Recover PlastiCZ's factory data from the identified i386 Mach-O slice.

This reads bytes; it never loads or executes the legacy plug-in. Addresses are
specific to the SHA-256 below. The instruction checks make the PIC calculations
reviewable and prevent accidental extraction from a different version.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import struct


ROOT = Path(__file__).resolve().parents[1]
EXPECTED_SHA256 = "aee3175bd128a69ee82c8e31a34a36284e21698f022c6f722a6c42f2fc94b635"
EVIDENCE_IDS = {
    "constructor_0x1454a": "ev_883025d7dfc4d48cbbc678e783d0a948d2d695bb07b138d5560530efa12d92c2",
    "get_parameter_name_0x131cc": "ev_f5e7485630b749115a6c0403301fe67cacc67b8e58e4765f83ccf6c417e1b8b5",
}


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


class MachO32:
    def __init__(self, data: bytes):
        self.data = data
        require(len(data) >= 28, "Truncated Mach-O header")
        magic, cpu, subtype, filetype, ncmds, sizeofcmds, flags = struct.unpack_from("<7I", data)
        require(magic == 0xFEEDFACE and cpu == 7 and filetype == 8,
                "Expected a little-endian i386 Mach-O bundle")
        self.segments = []
        command_offset = 28
        for _ in range(ncmds):
            cmd, cmdsize = struct.unpack_from("<2I", data, command_offset)
            require(cmdsize >= 8 and command_offset + cmdsize <= 28 + sizeofcmds,
                    "Invalid Mach-O load command")
            if cmd == 1:  # LC_SEGMENT, 32-bit
                name = data[command_offset + 8:command_offset + 24].split(b"\0")[0].decode("ascii")
                vmaddr, vmsize, fileoff, filesize = struct.unpack_from("<4I", data, command_offset + 24)
                require(fileoff + filesize <= len(data), "Segment exceeds file")
                self.segments.append(dict(name=name, vmaddr=vmaddr, vmsize=vmsize,
                                          fileoff=fileoff, filesize=filesize))
            command_offset += cmdsize

    def offset(self, address: int, size: int = 1) -> int:
        for segment in self.segments:
            delta = address - segment["vmaddr"]
            if 0 <= delta and delta + size <= segment["filesize"]:
                return segment["fileoff"] + delta
        raise ValueError(f"Address {address:#x} size {size:#x} is not file-backed")

    def read(self, address: int, size: int) -> bytes:
        offset = self.offset(address, size)
        return self.data[offset:offset + size]

    def u32(self, address: int) -> int:
        return struct.unpack("<I", self.read(address, 4))[0]

    def check(self, address: int, expected_hex: str) -> None:
        expected = bytes.fromhex(expected_hex)
        require(self.read(address, len(expected)) == expected,
                f"Unexpected instruction bytes at {address:#x}")

    def cstring(self, address: int) -> bytes:
        offset = self.offset(address)
        end = self.data.index(0, offset, offset + 128)
        return self.read(address, end - offset)


def recover(source: Path) -> tuple[dict, dict]:
    data = source.read_bytes()
    digest = sha256(data)
    require(digest == EXPECTED_SHA256, "Source SHA-256 differs; offsets are version-specific")
    macho = MachO32(data)
    for address, expected in {
        0x38F18: "8b1c24c3",          # mov ebx, [esp]; ret: PIC return-address helper
        0x14553: "e8c0490200",       # call 0x38f18; return address 0x14558
        0x1455B: "c744240c26000000", # constructor parameter count 38
        0x14563: "c744240880000000", # constructor program count 128
        0x1464D: "c7442408005c0000", # memcpy size 0x5c00
        0x14655: "8d83a86d0300",     # lea eax, [ebx+0x36da8]
        0x131D3: "e8405d0200",       # call 0x38f18; return address 0x131d8
        0x131DB: "8b848388800300",   # mov eax, [ebx+eax*4+0x38088]
        0x1468F: "c740685a434c50",   # unique ID 0x504c435a
    }.items():
        macho.check(address, expected)

    program_count = macho.u32(0x14567)
    parameter_count = macho.u32(0x1455F)
    bank_size = macho.u32(0x14651)
    bank_address = 0x14558 + macho.u32(0x14657)
    name_table_address = 0x131D8 + macho.u32(0x131DE)
    name_size = 32
    stride = name_size + parameter_count * 4
    require(program_count * stride == bank_size, "Program layout does not fill constructor memcpy")
    bank = macho.read(bank_address, bank_size)

    parameters = []
    for index in range(parameter_count):
        pointer_address = name_table_address + 4 * index
        name_address = macho.u32(pointer_address)
        raw_name = macho.cstring(name_address)
        parameters.append({
            "index": index,
            "name": raw_name.decode("ascii"),
            "name_bytes_hex": raw_name.hex(),
            "pointer_table_entry_address": hex(pointer_address),
            "name_address": hex(name_address),
            "name_file_offset": macho.offset(name_address),
            "program_value_byte_offset": name_size + 4 * index,
        })

    programs = []
    round_trip = bytearray()
    for index in range(program_count):
        relative_offset = index * stride
        raw = bank[relative_offset:relative_offset + stride]
        raw_name = raw[:name_size]
        require(0 in raw_name, f"Program {index} has no terminated name")
        name = raw_name.split(b"\0", 1)[0].decode("ascii")
        require(bool(name), f"Program {index} has an empty name")
        values = list(struct.unpack_from(f"<{parameter_count}f", raw, name_size))
        bits = list(struct.unpack_from(f"<{parameter_count}I", raw, name_size))
        require(all(math.isfinite(v) and 0 <= v <= 1 for v in values),
                f"Program {index} contains a non-finite or non-normalized value")
        reconstructed = raw_name + struct.pack(f"<{parameter_count}f", *values)
        require(reconstructed == raw, f"Program {index} float round trip differs")
        round_trip += reconstructed
        programs.append({
            "index": index,
            "name": name,
            "address": hex(bank_address + relative_offset),
            "file_offset": macho.offset(bank_address + relative_offset, stride),
            "name_storage_hex": raw_name.hex(),
            "values": values,
            "float32_bits_hex": [f"{value:08x}" for value in bits],
        })
    require(bytes(round_trip) == bank, "Bank round trip differs")
    for index, parameter in enumerate(parameters):
        values = [program["values"][index] for program in programs]
        parameter["factory_min"] = min(values)
        parameter["factory_max"] = max(values)

    provenance = {
        "source": "analysis/source/PlastiCZ-i386",
        "source_sha256": digest,
        "source_size_bytes": len(data),
        "source_format": "little-endian 32-bit i386 Mach-O bundle",
        "method": "Static byte extraction; original plug-in was not executed",
        "rea_evidence_file": "analysis/evidence/initial.rea.json",
        "rea_evidence_ids": EVIDENCE_IDS,
        "confidence": "High for literal bytes, record order, and host-visible parameter names",
        "derivation": {
            "pic_helper": "0x38f18: mov ebx,[esp]; ret",
            "factory_bank": "0x14558 + displacement at 0x14657 = 0x4b300",
            "parameter_name_table": "0x131d8 + displacement at 0x131de = 0x4b260",
            "program_count": "0x80 passed to constructor at 0x14563",
            "parameter_count": "0x26 passed to constructor at 0x1455b",
            "bank_size": "0x5c00 passed to memcpy at 0x1464d",
            "record_layout": "32 name bytes followed by 38 little-endian IEEE-754 floats; 128 * 184 = 0x5c00",
        },
        "limitations": [
            "Names are copied exactly, including dummy labels and trailing spaces; no expanded labels are inferred.",
            "Factory minima and maxima are observations of this bank, not parameter range specifications.",
            "Host labels alone do not establish DSP semantics or whether dummy parameters are used.",
            "Preset extraction does not validate sound or runtime compatibility of a reconstruction.",
            "Addresses and instruction checks apply only to the identified binary slice.",
        ],
    }
    factory = {
        "schema_version": 1,
        "provenance": provenance,
        "plugin_unique_id_u32": macho.u32(0x14692),
        "plugin_unique_id_hex": hex(macho.u32(0x14692)),
        "plugin_unique_id_fourcc": macho.u32(0x14692).to_bytes(4, "big").decode("ascii"),
        "program_count": program_count,
        "parameter_count": parameter_count,
        "bank_address": hex(bank_address),
        "bank_file_offset": macho.offset(bank_address, bank_size),
        "bank_size_bytes": bank_size,
        "bank_sha256": sha256(bank),
        "record_size_bytes": stride,
        "name_size_bytes": name_size,
        "parameter_map": "parameter-map.json",
        "validation": {
            "all_names_nonempty_terminated_ascii": True,
            "all_values_finite_within_zero_one": True,
            "all_records_float32_round_trip_exact": True,
            "bank_round_trip_sha256": sha256(round_trip),
            "value_count": program_count * parameter_count,
        },
        "programs": programs,
    }
    parameter_map = {
        "schema_version": 1,
        "provenance": provenance,
        "parameter_count": parameter_count,
        "table_address": hex(name_table_address),
        "table_file_offset": macho.offset(name_table_address, parameter_count * 4),
        "table_size_bytes": parameter_count * 4,
        "table_sha256": sha256(macho.read(name_table_address, parameter_count * 4)),
        "first_eight_labels": [p["name"] for p in parameters[:8]],
        "parameters": parameters,
    }
    return factory, parameter_map


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", nargs="?", type=Path,
                        default=ROOT / "analysis/source/PlastiCZ-i386")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "analysis/recovered")
    args = parser.parse_args()
    factory, parameter_map = recover(args.source)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    for filename, content in (("factory-presets.json", factory), ("parameter-map.json", parameter_map)):
        (args.output_dir / filename).write_text(json.dumps(content, indent=2, allow_nan=False) + "\n")
    print(f"Recovered {factory['program_count']} programs and {factory['parameter_count']} parameter names")
    print(f"Bank SHA-256: {factory['bank_sha256']}; byte-exact float32 round trip verified")


if __name__ == "__main__":
    main()
