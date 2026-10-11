#!/usr/bin/env python3
"""Experimental original i386 effect/output reference; no native DSP substitutions.

This extends the dry instruction oracle with the original DSP constructors,
sample-rate setter, parameter setter and effects dispatcher. Runtime allocation
and imported math are supplied by this process; unknown imports fail closed.
It does not instantiate the Carbon UI, VST host or license wrapper.
"""
from __future__ import annotations
import ctypes
import json
import math
import struct
from collections import Counter
from voice_oracle import VoiceOracle, OBJECT, OUT_LEFT, OUT_RIGHT, OUT_POINTERS, ROOT, SCRATCH
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_ESP

HEAP = 0x3000000
HEAP_SIZE = 0x2000000


class FullDSPOracle(VoiceOracle):
    def __init__(self):
        super().__init__()
        self.uc.mem_map(HEAP, HEAP_SIZE)
        self.heap_cursor = HEAP
        self.allocations = {}
        self.extended_calls = Counter()
        self.extra_imports = {
            0x6900A: ("free", 1), 0x6900F: ("malloc", 1),
            0x69023: ("calloc", 2), 0x69064: ("memcpy", 3),
            0x69195: ("realloc", 2),
            0x69348: ("sinf", 1), 0x6956E: ("cos", 1),
            0x69573: ("pow", 2), 0x69578: ("sin", 1),
            0x6957D: ("fmod", 2), 0x69587: ("floor", 1), 0x695FF: ("fmax", 2),
        }
        self.extra_math = {}
        for index, (address, (name, argc)) in enumerate(self.extra_imports.items()):
            if name in ("sinf", "cos", "pow", "sin", "fmod", "floor", "fmax"):
                is_double = not name.endswith("f")
                scalar = ctypes.c_double if is_double else ctypes.c_float
                function = getattr(self._libm, name)
                function.argtypes = [scalar] * argc
                function.restype = scalar
                result = SCRATCH + 0x600 + index * 0x20
                trampoline = result + 8
                self.extra_math[address] = (function, result, "d" if is_double else "f")
                opcode = b"\xdd\x05" if is_double else b"\xd9\x05"
                self.uc.mem_write(trampoline, opcode + struct.pack("<I", result) + b"\xc3")
                self.uc.mem_write(address, b"\xe9" + struct.pack("<i", trampoline - address - 5))
            else:
                self.uc.mem_write(address, b"\xc3")

    def allocate(self, count):
        if not 0 <= count <= HEAP_SIZE:
            raise ValueError("Invalid emulated allocation size")
        address = self.heap_cursor
        self.heap_cursor += (max(count, 1) + 15) & ~15
        if self.heap_cursor > HEAP + HEAP_SIZE:
            raise MemoryError("Emulated DSP heap exhausted")
        self.allocations[address] = count
        self.uc.mem_write(address, bytes(max(count, 1)))
        return address

    def _import(self, uc, address, size, data):
        if address not in getattr(self, "extra_imports", {}):
            return super()._import(uc, address, size, data)
        name, argc = self.extra_imports[address]
        self.extended_calls[name] += 1
        stack = uc.reg_read(UC_X86_REG_ESP)
        if address in self.extra_math:
            function, result, fmt = self.extra_math[address]
            args = struct.unpack("<" + fmt * argc, uc.mem_read(stack + 4, argc * (8 if fmt == "d" else 4)))
            uc.mem_write(result, struct.pack("<" + fmt, function(*args)))
            return
        args = struct.unpack("<" + "I" * argc, uc.mem_read(stack + 4, argc * 4))
        if name == "malloc":
            result = self.allocate(args[0])
        elif name == "calloc":
            result = self.allocate(args[0] * args[1])
        elif name == "free":
            # Individual fixtures own this bounded arena; reset releases it.
            result = 0
        elif name == "realloc":
            old, count = args
            if old and old not in self.allocations:
                raise ValueError("Unknown emulated realloc pointer")
            result = self.allocate(count)
            if old:
                uc.mem_write(result, bytes(uc.mem_read(old, min(count, self.allocations[old]))))
        elif name == "memcpy":
            target, source, count = args
            if count > HEAP_SIZE:
                raise ValueError("Invalid memcpy size")
            if count:
                uc.mem_write(target, bytes(uc.mem_read(source, count)))
            result = target
        else:
            raise RuntimeError("Unhandled original import " + name)
        uc.reg_write(UC_X86_REG_EAX, result)

    def initialize(self, parameters, sample_rate=44100.0):
        dry = list(parameters)
        dry[37] = 0.0
        super().initialize(dry, sample_rate)
        self.heap_cursor = HEAP
        self.allocations.clear()
        # Original sub_1454a vtable, constructors, and sub_14106's two objects.
        self.uc.mem_write(OBJECT, struct.pack("<I", 0x53B48))
        self.invoke(0x281A6, struct.pack("<I", OBJECT + 0x40EB18), fpcw=0x037F)
        self.invoke(0x2D85C, struct.pack("<I", OBJECT + 0x40EB94), fpcw=0x037F)
        effect = self.allocate(0x30C)
        self.invoke(0x25A8C, struct.pack("<I", effect), fpcw=0x037F)
        filter_object = self.allocate(0x160)
        self.invoke(0x25742, struct.pack("<I", filter_object), fpcw=0x037F)
        self.uc.mem_write(OBJECT + 0x40EB8C, struct.pack("<II", effect, filter_object))
        self.invoke(0x12C9E, struct.pack("<If", OBJECT, sample_rate), fpcw=0x037F)
        # Original parameter writes reset effect state on FX-type changes.
        for index, value in enumerate(parameters):
            self.invoke(0x12F4A, struct.pack("<IIf", OBJECT, index, value), fpcw=0x037F)
        # Host tempo defaults to120 in sub_1454a / sub_137f4's missing-time path.
        self.write_float(0x4006A0, 120.0)
        self.parameters = list(parameters)

    def render(self, frames, offset=0):
        # VoiceOracle calls the original scalar/4x render according to parameter37.
        super().render(frames, offset)
        self.invoke(0x10E4A, struct.pack("<IIII", OBJECT, OUT_POINTERS, offset, frames))
        return (bytes(self.uc.mem_read(OUT_LEFT + 4 * offset, frames * 4)),
                bytes(self.uc.mem_read(OUT_RIGHT + 4 * offset, frames * 4)))


if __name__ == "__main__":
    factory = json.loads((ROOT / "analysis/recovered/factory-presets.json").read_text())["programs"]
    oracle = FullDSPOracle()
    for index in (0, 8, 32, 64, 96):
        print("Initializing", index, flush=True)
        oracle.initialize(factory[index]["values"])
        oracle.control_tick()
        oracle.note_on(24, 100 / 127)
        peak = 0.0
        for _ in range(200):
            oracle.control_tick()
            left, right = oracle.render(44)
            samples = struct.unpack("<88f", left + right)
            if not all(math.isfinite(x) for x in samples):
                raise RuntimeError("Non-finite full output")
            peak = max(peak, max(abs(x) for x in samples))
        print("Preset", index, "peak", peak, "imports", dict(oracle.extended_calls), flush=True)
