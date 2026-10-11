#!/usr/bin/env python3
"""Run original i386 PlastiCZ voice/control/dry-render instructions in isolation.

This deliberately does not instantiate the legacy VST, GUI, or license wrapper.
The backing object is zeroed and its core fields are initialized from observed
constructor instructions. cosf/expf/powf use the current host's libSystem, so
fixtures are an instruction oracle with that explicit libm boundary, not a
recording of the original plug-in running in an old macOS host.
"""
from __future__ import annotations

import argparse
from collections import Counter
import ctypes
import hashlib
import json
import math
from pathlib import Path
import struct

from unicorn import UcError, UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_EIP, UC_X86_REG_ESP

from x86_oracle import X86Oracle, DEFAULT_BINARY, ROOT, SCRATCH, STACK_TOP, DSP_FPCW, f32

OBJECT = 0x1000000
OBJECT_SIZE = 0x410C34
OBJECT_MAPPED = 0x411000
IO = 0x2000000
IO_SIZE = 0x200000
OUT_POINTERS = IO
OUT_LEFT = IO + 0x1000
OUT_RIGHT = IO + 0x81000
OVERSAMPLE = IO + 0x101000
MAX_FRAMES = 32768
VOID_RETURN = SCRATCH + 0x200
VOICE_OFFSET = 0x40635C
VOICE_STRIDE = 0x5C
VOICE_COUNT = 16
PARAM_OFFSET = 0x4069AC


class VoiceOracle(X86Oracle):
    def __init__(self, binary: Path = DEFAULT_BINARY):
        super().__init__(binary)
        self.uc.mem_map(OBJECT, OBJECT_MAPPED)
        self.uc.mem_map(IO, IO_SIZE)
        self.uc.mem_write(VOID_RETURN, b"\x90")
        self.import_calls = Counter()
        self.import_error = None
        self._libm = ctypes.CDLL("/usr/lib/libSystem.B.dylib")
        self.imports = {0x69028: ("memset", 3), 0x693ED: ("memmove", 3),
                        0x69352: ("cosf", 1), 0x693E3: ("powf", 2),
                        0x693E8: ("expf", 1)}
        self._float_results = {}
        for index, (address, (name, argc)) in enumerate(self.imports.items()):
            if name.endswith("f"):
                function = getattr(self._libm, name)
                function.argtypes = [ctypes.c_float] * argc
                function.restype = ctypes.c_float
                result = SCRATCH + 0x300 + index * 0x20
                trampoline = result + 4
                self._float_results[address] = (function, result)
                self.uc.mem_write(trampoline, b"\xd9\x05" + struct.pack("<I", result) + b"\xc3")
                # Original import slots are only five bytes apart. Redirect to
                # scratch instead of overwriting a neighbour with FLD+RET.
                self.uc.mem_write(address, b"\xe9" + struct.pack("<i", trampoline - address - 5))
            else:
                self.uc.mem_write(address, b"\xc3")
        self.uc.hook_add(UC_HOOK_CODE, self._import, begin=0x69000, end=0x69FFF)

    def _import(self, uc, address, size, _):
        if address not in self.imports:
            self.import_error = f"Unimplemented original import at {address:#x}"
            uc.emu_stop()
            return
        name, argc = self.imports[address]
        self.import_calls[name] += 1
        stack = uc.reg_read(UC_X86_REG_ESP)
        arguments = bytes(uc.mem_read(stack + 4, argc * 4))
        if address in self._float_results:
            function, result = self._float_results[address]
            values = struct.unpack("<" + "f" * argc, arguments)
            uc.mem_write(result, struct.pack("<f", function(*values)))
        else:
            target, source_or_value, count = struct.unpack("<III", arguments)
            if count > OBJECT_SIZE:
                self.import_error = f"Unexpected {name} length {count}"
                uc.emu_stop()
                return
            data = bytes([source_or_value & 255]) * count if name == "memset" else bytes(uc.mem_read(source_or_value, count))
            if count:
                uc.mem_write(target, data)
            uc.reg_write(UC_X86_REG_EAX, target)

    def invoke(self, address: int, arguments: bytes, *, fpcw: int = DSP_FPCW,
               budget: int = 20_000_000) -> int:
        self._reset_registers(fpcw)
        self.uc.mem_write(STACK_TOP, struct.pack("<I", VOID_RETURN) + arguments)
        self.import_error = None
        try:
            self.uc.emu_start(address, VOID_RETURN, count=budget)
        except UcError as error:
            pc = self.uc.reg_read(UC_X86_REG_EIP)
            raise RuntimeError(f"Original call {address:#x} failed at {pc:#x}: {error}") from error
        if self.import_error:
            raise RuntimeError(self.import_error)
        if self.uc.reg_read(UC_X86_REG_EIP) != VOID_RETURN:
            raise RuntimeError(f"Original call {address:#x} exhausted its instruction budget")
        return self.uc.reg_read(UC_X86_REG_EAX)

    def write_float(self, offset: int, value: float):
        self.uc.mem_write(OBJECT + offset, struct.pack("<f", value))

    def read_float(self, offset: int) -> float:
        return struct.unpack("<f", self.uc.mem_read(OBJECT + offset, 4))[0]

    def initialize(self, parameters: list[float], sample_rate: float = 44100.0):
        if len(parameters) != 38 or not all(math.isfinite(x) and 0 <= x <= 1 for x in parameters):
            raise ValueError("Expected 38 finite normalized parameters")
        if parameters[37] >= 0.5:
            raise ValueError("This fixture oracle currently supports the dry non-oversampled core only")
        if not math.isfinite(sample_rate) or sample_rate <= 0:
            raise ValueError("Sample rate must be positive and finite")
        self.uc.mem_write(OBJECT, bytes(OBJECT_MAPPED))
        self.uc.mem_write(IO, bytes(IO_SIZE))
        self.uc.mem_write(OUT_POINTERS, struct.pack("<II", OUT_LEFT, OUT_RIGHT))
        self.uc.mem_write(OBJECT + 0x40EB14, struct.pack("<I", OVERSAMPLE))
        self.parameters = [f32(value) for value in parameters]
        self.sample_rate = f32(sample_rate)
        # Scalar portion of original 0x12c9e sample-rate setter. Optional effects
        # and embedded dynamics objects are outside this isolated dry core.
        self.write_float(0xC, self.sample_rate)
        self.write_float(0x406344, self.sample_rate)
        self.write_float(0x406348, f32(f32(1.0 / self.sample_rate) * 44100.0))
        self.invoke(0xED14, struct.pack("<I", OBJECT), fpcw=0x037F)
        # Original constructor scalar writes: 0x1417d..0x14240. Zero fields,
        # voices and held-key stack have already been set by the object clear.
        self.uc.mem_write(OBJECT + 0x406328, b"\x7f\x7f")
        self.uc.mem_write(OBJECT + 0x406354, struct.pack("<I", 0xFFFFFFFF))
        self.uc.mem_write(OBJECT + 0x410C30, b"\x01")
        for offset, value in ((0x40632C, 0.5), (0x406330, 0.5),
                              (0x40633C, 1.0), (0x406340, 1.0)):
            self.write_float(offset, value)
        self.uc.mem_write(OBJECT + PARAM_OFFSET, struct.pack("<38f", *self.parameters))

    def note_on(self, note: int, velocity: float, channel: int = 0):
        self.invoke(0xF5E4, struct.pack("<IIIf", OBJECT, channel, note, velocity))

    def note_off(self, note: int, channel: int = 0):
        self.invoke(0xF3F2, struct.pack("<III", OBJECT, channel, note))

    def control_tick(self):
        self.invoke(0x10534, struct.pack("<I", OBJECT))

    def voices(self) -> bytes:
        return bytes(self.uc.mem_read(OBJECT + VOICE_OFFSET, VOICE_COUNT * VOICE_STRIDE))

    def voice_words(self, voice=0) -> list[int]:
        return list(struct.unpack("<23I", self.voices()[voice * VOICE_STRIDE:(voice + 1) * VOICE_STRIDE]))

    def render(self, frames: int, offset: int = 0) -> bytes:
        if frames < 0 or offset < 0 or frames + offset > MAX_FRAMES:
            raise ValueError("Render range outside deterministic output buffer")
        if frames:
            self.uc.mem_write(OUT_LEFT, bytes((frames + offset) * 4))
            self.uc.mem_write(OUT_RIGHT, bytes((frames + offset) * 4))
        self.invoke(0xF8D0, struct.pack("<IIII", OBJECT, OUT_POINTERS, offset, frames))
        return bytes(self.uc.mem_read(OUT_LEFT + 4 * offset, frames * 4))


def export(output: Path, binary: Path = DEFAULT_BINARY) -> dict:
    output.mkdir(parents=True, exist_ok=True)
    presets = json.loads((ROOT / "analysis/recovered/factory-presets.json").read_text())["programs"]
    oracle = VoiceOracle(binary)
    cases = []
    states = bytearray()
    audio = bytearray()
    # Include contrasting factory sounds, both supported rates, overlapping
    # notes, note-off, and enough ticks for attack/decay/release transitions.
    for preset_index, rate in ((0, 44100), (0, 48000), (8, 44100), (32, 44100), (64, 48000), (96, 44100)):
        preset = presets[preset_index]
        parameters = preset["values"].copy()
        parameters[37] = 0.0
        oracle.initialize(parameters, rate)
        case = {"id": f"preset-{preset_index:03d}-{rate}", "factory_index": preset_index,
                "factory_name": preset["name"], "sample_rate": rate,
                "parameters": oracle.parameters, "parameter_overrides": {"37": 0.0}, "steps": []}

        def capture(action, **fields):
            record = {"action": action, **fields, "voices_byte_offset": len(states)}
            states.extend(oracle.voices())
            case["steps"].append(record)

        capture("initialize")
        # Configure mono/poly before note-on, as the process control step does.
        oracle.control_tick()
        capture("control")
        oracle.note_on(60, f32(100.0 / 127.0))
        capture("note_on", note=60, velocity=f32(100.0 / 127.0), channel=0)
        for tick in range(160):
            if tick == 20:
                oracle.note_on(67, f32(80.0 / 127.0))
                capture("note_on", note=67, velocity=f32(80.0 / 127.0), channel=0)
            if tick in (45, 70):
                note = 60 if tick == 45 else 67
                oracle.note_off(note)
                capture("note_off", note=note, channel=0)
            oracle.control_tick()
            capture("control")
            frames = 32
            samples = oracle.render(frames)
            values = struct.unpack("<" + "f" * frames, samples)
            if not all(math.isfinite(x) for x in values):
                raise RuntimeError(f"Non-finite audio in {case['id']} tick {tick}")
            start = len(audio)
            audio.extend(samples)
            capture("render", frames=frames, offset=0, audio_byte_offset=start)
        cases.append(case)
    (output / "voices.bin").write_bytes(states)
    (output / "audio-mono.f32").write_bytes(audio)
    manifest = {
        "schema_version": 1, "source_sha256": oracle.sha256,
        "kind": "original-i386-voice-control-dry-render-instruction-fixtures",
        "x87_control_word": hex(DSP_FPCW), "sse_mxcsr": "0x1f80",
        "voice_count": VOICE_COUNT, "voice_stride_bytes": VOICE_STRIDE,
        "voice_storage": "voices.bin", "voice_byte_order": "little",
        "audio_storage": "audio-mono.f32", "audio_encoding": "IEEE-754 float32 little-endian mono",
        "states_sha256": hashlib.sha256(states).hexdigest(),
        "audio_sha256": hashlib.sha256(audio).hexdigest(),
        "import_calls": dict(oracle.import_calls),
        "initialization": "Zeroed object; scalar sample-rate writes from 0x12c9e; original initializer 0xed14; scalar constructor fields from 0x1417d..0x14240; all 38 preset floats applied directly.",
        "execution": {"note_on": "0xf5e4(object, channel, note, float_velocity)",
                      "note_off": "0xf3f2(object, channel, note)",
                      "control": "0x10534(object)",
                      "render": "0xf8d0(object, float** outputs, offset, frames)"},
        "limitations": [
            "The original plug-in has not run in a legacy macOS DAW; this is isolated instruction execution.",
            "Imported cosf, expf and powf use current ARM libSystem and can differ from historical x86 libm.",
            "Control/render sequencing is explicitly scripted, not a claim about original host block scheduling.",
            "Original effects, output postprocessing, oversampling and host/GUI behavior are outside these dry core fixtures.",
            "Parameter 37 is forced to zero to select the original non-oversampled core.",
        ],
        "cases": cases,
    }
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    # Deliberately simple companion format for a dependency-free C++ comparator.
    # The JSON manifest is authoritative; this repeats only its ordered actions.
    commands = ["PLASTICZ_VOICE_ORACLE 1"]
    for case in cases:
        commands.append(f"case {case['id']} {case['sample_rate']}")
        commands.append("parameters " + " ".join(format(x, ".9g") for x in case["parameters"]))
        for step in case["steps"]:
            fields = [step["action"], str(step["voices_byte_offset"])]
            if step["action"] == "note_on":
                fields += [str(step["note"]), format(step["velocity"], ".9g"), str(step["channel"])]
            elif step["action"] == "note_off":
                fields += [str(step["note"]), str(step["channel"])]
            elif step["action"] == "render":
                fields += [str(step["frames"]), str(step["offset"]), str(step["audio_byte_offset"])]
            commands.append(" ".join(fields))
        commands.append("end")
    (output / "commands.txt").write_text("\n".join(commands) + "\n")
    return {"cases": len(cases), "state_records": len(states) // (VOICE_COUNT * VOICE_STRIDE),
            "mono_samples": len(audio) // 4, "import_calls": dict(oracle.import_calls),
            "output": str(output)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=DEFAULT_BINARY)
    parser.add_argument("--output", type=Path, default=ROOT / "analysis/recovered/voice-oracle")
    args = parser.parse_args()
    print(json.dumps(export(args.output, args.binary), indent=2))


if __name__ == "__main__":
    main()
