#!/usr/bin/env python3
"""Execute PlastiCZ's original i386 waveform functions in Unicorn.

This is an isolated instruction-level oracle, not execution in a legacy macOS
host. No original plug-in entry point, constructor, GUI or licensing code runs.
The cosine initializer's sole imported function, cosf, is supplied by the current
host's libSystem. Exporting the table lets a native port use exactly the same
table and separates waveform-algorithm agreement from old libm differences.
"""

from __future__ import annotations

import argparse
import csv
import ctypes
import hashlib
import json
from pathlib import Path
import struct

import unicorn
from unicorn import Uc, UcError, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn.x86_const import (
    UC_X86_REG_EAX, UC_X86_REG_EBP, UC_X86_REG_EBX, UC_X86_REG_ECX,
    UC_X86_REG_EDI, UC_X86_REG_EDX, UC_X86_REG_EFLAGS, UC_X86_REG_EIP,
    UC_X86_REG_ESI, UC_X86_REG_ESP, UC_X86_REG_FPCW, UC_X86_REG_FPSW,
    UC_X86_REG_FPTAG, UC_X86_REG_MXCSR,
)

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BINARY = ROOT / "analysis/source/PlastiCZ-i386"
SOURCE_SHA256 = "aee3175bd128a69ee82c8e31a34a36284e21698f022c6f722a6c42f2fc94b635"
WAVE_NAMES = ("Saw", "Square", "Pulse", "SawPulse", "DblCos", "Reso", "ResoSqu", "DblSaw")
WAVE_TABLE = 0x4B140
OFFSET_TABLE = 0x4B120
COS_TABLE = 0x58400
COS_COUNT = 0x4000
COS_INIT = 0xED14
COS_INIT_END = 0xED5C
COSF_STUB = 0x69352
SCRATCH = 0x100000
RETURN_TRAMPOLINE = SCRATCH
RESULT = SCRATCH + 0x100
COSF_RESULT = SCRATCH + 0x104
STACK = 0x200000
STACK_SIZE = 0x10000
STACK_TOP = STACK + STACK_SIZE - 0x100
# Established in the original process path: 24-bit x87 precision, round-to-zero.
DSP_FPCW = 0x0C3F


def f32(value: float) -> float:
    return struct.unpack("<f", struct.pack("<f", value))[0]


class X86Oracle:
    def __init__(self, binary: Path = DEFAULT_BINARY):
        self.binary = Path(binary)
        data = self.binary.read_bytes()
        self.sha256 = hashlib.sha256(data).hexdigest()
        if self.sha256 != SOURCE_SHA256:
            raise ValueError("Unsupported binary fingerprint; addresses are specific to the recovered PlastiCZ image")
        self.uc = Uc(UC_ARCH_X86, UC_MODE_32)
        # Text, initialized data, BSS and import stubs; __LINKEDIT is not executed.
        self.uc.mem_map(0, 0x6A000)
        self.segments = self._load_segments(data)
        self.uc.mem_map(SCRATCH, 0x1000)
        self.uc.mem_map(STACK, STACK_SIZE)
        # Original float return is ST0. Store it with an actual emulated FSTP.
        self.uc.mem_write(RETURN_TRAMPOLINE, b"\xd9\x1d" + struct.pack("<I", RESULT))
        self.waves = struct.unpack("<8I", self.uc.mem_read(WAVE_TABLE, 32))
        self.offsets = struct.unpack("<8I", self.uc.mem_read(OFFSET_TABLE, 32))
        assert self.waves == (0xEF38, 0xEFBE, 0xF072, 0xF0FA, 0xF182, 0xF20A, 0xF2A0, 0xF34A)
        assert self.offsets == (0xEE9A, 0xEEA2, 0xEEAA, 0xEEB2, 0xEED4, 0xEEFE, 0xEF0E, 0xEEDC)
        self.initialize_cosine_table()

    def _load_segments(self, data: bytes) -> list[dict]:
        header = struct.unpack_from("<7I", data)
        if header[0] != 0xFEEDFACE or header[1] != 7:
            raise ValueError("Expected little-endian 32-bit i386 Mach-O")
        cursor = 28
        segments = []
        for _ in range(header[4]):
            command, size = struct.unpack_from("<II", data, cursor)
            if command == 1:  # LC_SEGMENT
                fields = struct.unpack_from("<II16s8I", data, cursor)
                name = fields[2].split(b"\0", 1)[0].decode("ascii")
                addr, vmsize, fileoff, filesize = fields[3:7]
                sections = fields[9]
                if name != "__LINKEDIT":
                    if addr + vmsize > 0x6A000:
                        raise ValueError("Unexpected segment outside mapped image")
                    self.uc.mem_write(addr, data[fileoff:fileoff + filesize])
                    # S_ZEROFILL sections overlap padding of __DATA's file range.
                    for i in range(sections):
                        sec = struct.unpack_from("<16s16s9I", data, cursor + 56 + 68 * i)
                        sec_addr, sec_size, flags = sec[2], sec[3], sec[8]
                        if flags & 0xFF == 1:
                            self.uc.mem_write(sec_addr, bytes(sec_size))
                    segments.append(dict(name=name, address=hex(addr), size=vmsize,
                                         file_offset=fileoff, file_size=filesize))
            cursor += size
        return segments

    def _reset_registers(self, fpcw: int = DSP_FPCW) -> None:
        for reg in (UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_ECX,
                    UC_X86_REG_EDX, UC_X86_REG_ESI, UC_X86_REG_EDI, UC_X86_REG_EBP):
            self.uc.reg_write(reg, 0)
        self.uc.reg_write(UC_X86_REG_ESP, STACK_TOP)
        self.uc.reg_write(UC_X86_REG_EFLAGS, 2)
        self.uc.reg_write(UC_X86_REG_FPCW, fpcw)
        self.uc.reg_write(UC_X86_REG_FPSW, 0)
        self.uc.reg_write(UC_X86_REG_FPTAG, 0xFFFF)
        self.uc.reg_write(UC_X86_REG_MXCSR, 0x1F80)  # SSE round-to-nearest.

    def initialize_cosine_table(self) -> None:
        """Run only the original table-initialization loop, stopping before object writes."""
        libm = ctypes.CDLL("/usr/lib/libSystem.B.dylib")
        cosf = libm.cosf
        cosf.argtypes = [ctypes.c_float]
        cosf.restype = ctypes.c_float
        self.cosf_calls = 0
        original_stub = bytes(self.uc.mem_read(COSF_STUB, 7))
        # FLD dword [COSF_RESULT]; RET. Hook only supplies the imported result.
        self.uc.mem_write(COSF_STUB, b"\xd9\x05" + struct.pack("<I", COSF_RESULT) + b"\xc3")

        def imported_cosf(uc, address, size, user_data):
            arg = struct.unpack("<f", uc.mem_read(uc.reg_read(UC_X86_REG_ESP) + 4, 4))[0]
            uc.mem_write(COSF_RESULT, struct.pack("<f", cosf(arg)))
            self.cosf_calls += 1

        hook = self.uc.hook_add(UC_HOOK_CODE, imported_cosf, begin=COSF_STUB, end=COSF_STUB)
        self._reset_registers(0x037F)
        self.uc.mem_write(STACK_TOP, struct.pack("<I", RETURN_TRAMPOLINE))
        try:
            self.uc.emu_start(COS_INIT, COS_INIT_END, count=1_000_000)
            if self.uc.reg_read(UC_X86_REG_EIP) != COS_INIT_END or self.cosf_calls != COS_COUNT:
                raise RuntimeError("Cosine initializer did not complete the expected 16384 iterations")
        finally:
            self.uc.hook_del(hook)
            self.uc.mem_write(COSF_STUB, original_stub)
        self.cosine_bytes = bytes(self.uc.mem_read(COS_TABLE, COS_COUNT * 4))

    def call_bits(self, address: int, *float_args: float) -> int:
        """cdecl scalar call with float arguments and float return in x87 ST0."""
        self._reset_registers()
        self.uc.mem_write(STACK_TOP, struct.pack("<I", RETURN_TRAMPOLINE)
                          + b"".join(struct.pack("<f", x) for x in float_args))
        self.uc.mem_write(RESULT, b"\xff" * 4)
        try:
            self.uc.emu_start(address, RETURN_TRAMPOLINE + 6, count=10000)
        except UcError as exc:
            pc = self.uc.reg_read(UC_X86_REG_EIP)
            raise RuntimeError(f"x86 call {address:#x}{float_args} failed at {pc:#x}: {exc}") from exc
        if self.uc.reg_read(UC_X86_REG_EIP) != RETURN_TRAMPOLINE + 6:
            raise RuntimeError(f"x86 call {address:#x} exceeded instruction budget")
        return struct.unpack("<I", self.uc.mem_read(RESULT, 4))[0]

    def call(self, address: int, *float_args: float) -> float:
        return struct.unpack("<f", struct.pack("<I", self.call_bits(address, *float_args)))[0]

    def waveform(self, wave: int, phase: float, distortion: float) -> float:
        return self.call(self.waves[wave], phase, distortion)

    def offset(self, wave: int, distortion: float) -> float:
        return self.call(self.offsets[wave], distortion)

    def export(self, fixtures: Path, table: Path, phase_steps: int = 256, distortion_steps: int = 16) -> dict:
        if phase_steps < 1 or distortion_steps < 1:
            raise ValueError("Grid step counts must be positive")
        fixtures.parent.mkdir(parents=True, exist_ok=True)
        table.parent.mkdir(parents=True, exist_ok=True)
        table.write_bytes(self.cosine_bytes)
        rows = 0
        with fixtures.open("w", newline="") as handle:
            out = csv.writer(handle)
            out.writerow(("kind", "wave", "phase", "distortion", "expected", "expected_bits"))
            for wave in range(8):
                for d in range(distortion_steps + 1):
                    distortion = f32(d / distortion_steps)
                    bits = self.call_bits(self.offsets[wave], distortion)
                    value = struct.unpack("<f", struct.pack("<I", bits))[0]
                    out.writerow(("offset", wave, 0, format(distortion, ".9g"), format(value, ".9g"), f"0x{bits:08x}"))
                    rows += 1
                    for p in range(phase_steps + 1):
                        phase = f32(p / phase_steps)
                        bits = self.call_bits(self.waves[wave], phase, distortion)
                        value = struct.unpack("<f", struct.pack("<I", bits))[0]
                        out.writerow(("wave", wave, format(phase, ".9g"), format(distortion, ".9g"),
                                      format(value, ".9g"), f"0x{bits:08x}"))
                        rows += 1
        metadata = {
            "source": str(self.binary), "source_sha256": self.sha256,
            "unicorn_version": unicorn.__version__, "architecture": "i386",
            "x87_control_word": hex(DSP_FPCW), "sse_mxcsr": "0x1f80",
            "wave_names": WAVE_NAMES, "wave_functions": [hex(x) for x in self.waves],
            "offset_functions": [hex(x) for x in self.offsets], "segments": self.segments,
            "phase_steps": phase_steps, "distortion_steps": distortion_steps, "rows": rows,
            "cosine_table_address": hex(COS_TABLE), "cosine_table_samples": COS_COUNT,
            "cosine_table_sha256": hashlib.sha256(self.cosine_bytes).hexdigest(),
            "cosine_initializer": "original instructions 0xed14..0xed5c; imported cosf supplied by host libSystem",
            "limitations": [
                "Instruction emulation is not validation in the original legacy macOS/DAW host.",
                "Host ARM libSystem cosf may differ in final bits from original x86 macOS cosf.",
                "Only waveform/DC-offset functions and the isolated table loop execute; no voice, envelope, effect or host behavior is established.",
            ],
        }
        fixtures.with_suffix(".metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
        return metadata


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=DEFAULT_BINARY)
    parser.add_argument("--fixtures", type=Path, default=ROOT / "analysis/recovered/waveform-oracle.csv")
    parser.add_argument("--table", type=Path, default=ROOT / "analysis/recovered/cosine-table.f32")
    parser.add_argument("--phase-steps", type=int, default=256)
    parser.add_argument("--distortion-steps", type=int, default=16)
    args = parser.parse_args()
    result = X86Oracle(args.binary).export(args.fixtures, args.table, args.phase_steps, args.distortion_steps)
    print(json.dumps({k: result[k] for k in ("rows", "source_sha256", "cosine_table_sha256")}, indent=2))


if __name__ == "__main__":
    main()
