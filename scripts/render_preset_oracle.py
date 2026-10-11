#!/usr/bin/env python3
"""Render deterministic PlastiCZ factory-preset WAVs with original i386 DSP.

Uses the isolated instruction oracle, not a legacy macOS host. Original dry
voice/control/render routines run under Unicorn; cosf/expf/powf are pinned to
this host's ARM libSystem. Effects and oversampling are outside this suite.
"""
from __future__ import annotations

import argparse
import array
import hashlib
import json
import math
from pathlib import Path
import struct
import sys
import time

from voice_oracle import VoiceOracle, OBJECT, ROOT, f32
from x86_oracle import DEFAULT_BINARY, DSP_FPCW


PROTOCOL = "plasticz-original-dry-factory-c1-v1"


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def canonical(document) -> bytes:
    return json.dumps(document, sort_keys=True, separators=(",", ":"), allow_nan=False).encode()


def write_json(path: Path, document):
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(document, indent=2, allow_nan=False) + "\n")
    temporary.replace(path)


def float_wav(payload: bytes, sample_rate: int) -> bytes:
    if len(payload) % 4:
        raise ValueError("Float32 WAV payload must consist of complete samples")
    fmt = struct.pack("<HHIIHH", 3, 1, sample_rate, sample_rate * 4, 4, 32)
    return (b"RIFF" + struct.pack("<I", 48 + len(payload)) + b"WAVE"
            + b"fmt " + struct.pack("<I", 16) + fmt
            + b"fact" + struct.pack("<II", 4, len(payload) // 4)
            + b"data" + struct.pack("<I", len(payload)) + payload)


def wav_payload(wav: bytes) -> bytes:
    if len(wav) < 12 or wav[:4] != b"RIFF" or wav[8:12] != b"WAVE":
        raise ValueError("Expected a RIFF WAV")
    if struct.unpack_from("<I", wav, 4)[0] != len(wav) - 8:
        raise ValueError("Inconsistent RIFF size")
    cursor = 12
    format_seen = False
    while cursor + 8 <= len(wav):
        tag, size = wav[cursor:cursor + 4], struct.unpack_from("<I", wav, cursor + 4)[0]
        start, end = cursor + 8, cursor + 8 + size
        if end > len(wav):
            raise ValueError("WAV chunk exceeds file")
        if tag == b"fmt ":
            encoding, channels, _, _, align, bits = struct.unpack_from("<HHIIHH", wav, start)
            if (encoding, channels, align, bits) != (3, 1, 4, 32):
                raise ValueError("Expected mono IEEE float32 WAV")
            format_seen = True
        if tag == b"data" and format_seen:
            return wav[start:end]
        cursor = end + (size & 1)
    raise ValueError("No IEEE float32 WAV data")


class OriginalScheduler:
    """Scalar scheduling reconstruction of original 0x13a1b..0x13aa7.

    The wrapper only schedules calls to original routines. It does not perform
    any audio or envelope calculations. Countdown uses the original object's
    +0x406a44 field and float32 SSE operation order at every host boundary.
    """
    def __init__(self, oracle: VoiceOracle, sample_rate: int):
        self.oracle = oracle
        self.period = f32(f32(0.0010000000474974513) * f32(sample_rate))
        self.ticks = 0
        self.render_calls = 0

    def render(self, frames: int) -> bytes:
        result = bytearray()
        remaining = frames
        while remaining:
            countdown = self.oracle.read_float(0x406A44)
            until_tick = int(countdown)
            count = min(until_tick, remaining)
            if count < 0:
                raise RuntimeError("Unexpected negative original control countdown")
            if count:
                result.extend(self.oracle.render(count))
                self.render_calls += 1
            remaining -= count
            countdown = f32(countdown - f32(count))
            if count == until_tick:
                countdown = f32(countdown + self.period)
                self.oracle.write_float(0x406A44, countdown)
                self.oracle.control_tick()
                self.ticks += 1
            else:
                self.oracle.write_float(0x406A44, countdown)
        return bytes(result)


def render_case(oracle: VoiceOracle, parameters: list[float], config: dict) -> tuple[bytes, dict]:
    oracle.initialize(parameters, config["sample_rate"])
    scheduler = OriginalScheduler(oracle, config["sample_rate"])
    # Actual scheduler preroll initializes mono mode without an artificial
    # standalone tick. Its fractional control countdown carries into the note.
    preroll = scheduler.render(config["preroll_frames"])
    if any(wav_sample != 0 for wav_sample in struct.unpack("<" + "f" * (len(preroll) // 4), preroll)):
        raise RuntimeError("Expected silent no-note preroll")
    start_countdown = oracle.read_float(0x406A44)
    oracle.note_on(config["note"], config["velocity"], config["channel"])
    result = bytearray()
    total = config["hold_frames"] + config["release_frames"]
    frame = 0
    note_released = False
    host_blocks = 0
    while frame < total:
        block_end = min(total, frame + config["block_size"])
        host_blocks += 1
        while frame < block_end:
            if frame == config["hold_frames"] and not note_released:
                oracle.note_off(config["note"], config["channel"])
                note_released = True
            boundary = block_end
            if not note_released:
                boundary = min(boundary, config["hold_frames"])
            result.extend(scheduler.render(boundary - frame))
            frame = boundary
    if not note_released:
        oracle.note_off(config["note"], config["channel"])
    if len(result) != total * 4:
        raise RuntimeError("Unexpected render payload length")
    values = array.array("f")
    values.frombytes(result)
    if sys.byteorder != "little":
        values.byteswap()
    if not all(math.isfinite(value) for value in values):
        raise RuntimeError("Non-finite original audio")
    stats = {"frames": total, "host_blocks": host_blocks,
             "control_ticks_including_preroll": scheduler.ticks,
             "original_render_calls_including_preroll": scheduler.render_calls,
             "countdown_at_note_on": start_countdown,
             "countdown_at_end": oracle.read_float(0x406A44),
             "peak": max(map(abs, values), default=0),
             "rms": math.sqrt(sum(float(value) * value for value in values) / max(len(values), 1)),
             "nonzero_samples": sum(value != 0 for value in values)}
    return bytes(result), stats


def parse_presets(value: str) -> list[int]:
    if value == "all":
        return list(range(128))
    result = []
    for part in value.split(","):
        endpoints = part.split("-")
        if len(endpoints) == 1:
            result.append(int(part))
        elif len(endpoints) == 2:
            result.extend(range(int(endpoints[0]), int(endpoints[1]) + 1))
        else:
            raise ValueError("Use all, a comma-separated list, or inclusive ranges")
    if not result or any(index < 0 or index > 127 for index in result):
        raise ValueError("Preset indexes must be in 0..127")
    return sorted(set(result))


def suite(args) -> dict:
    if args.rate < 8000 or args.rate > 384000 or args.block_size < 1 or args.block_size > 32768:
        raise ValueError("Unsupported rate or host block size")
    if not all(math.isfinite(value) and value >= 0 for value in (args.hold_seconds, args.release_seconds)):
        raise ValueError("Durations must be finite and nonnegative")
    hold_frames, release_frames = round(args.hold_seconds * args.rate), round(args.release_seconds * args.rate)
    if hold_frames < 1 or hold_frames + release_frames > args.rate * 300:
        raise ValueError("Hold must contain samples; total duration must be <=300 seconds")
    selected = parse_presets(args.presets)
    factory_path = ROOT / "analysis/recovered/factory-presets.json"
    factory_bytes = factory_path.read_bytes()
    factory = json.loads(factory_bytes)
    config = {"protocol": PROTOCOL, "sample_rate": args.rate, "block_size": args.block_size,
              "hold_frames": hold_frames, "release_frames": release_frames,
              "preroll_frames": args.block_size, "note": 24, "velocity": f32(100.0 / 127.0),
              "channel": 0, "channels": 1, "wav_format": "IEEE float32 little-endian (format tag 3)",
              "parameter_overrides": {"37": 0.0}, "effects": "original outer effects bypassed",
              "event_timing": "One discarded silent host block through persistent original scheduler, note_on before emitted frame 0, note_off before emitted hold_frames; split host block exactly at note-off; retain original host block boundaries and float32 countdown.",
              "scheduler": "Original +0x406a44 countdown, float32; int truncation; dry render up to boundary; float32 subtract count; add float32(0.0010000000474974513*sample_rate), then original control0x10534. Includes tick at end-of-block if exactly due.",
              "source_sha256": sha(args.binary.read_bytes()), "factory_json_sha256": sha(factory_bytes),
              "implementation_sha256": sha(Path(__file__).read_bytes()),
              "voice_oracle_sha256": sha((ROOT / "scripts/voice_oracle.py").read_bytes()),
              "scalar_oracle_sha256": sha((ROOT / "scripts/x86_oracle.py").read_bytes()),
              "x87_control_word": hex(DSP_FPCW), "sse_mxcsr": "0x1f80",
              "math_boundary": "Current host ARM /usr/lib/libSystem.B.dylib cosf, expf, powf"}
    config_hash = sha(canonical(config))
    directory = args.output
    reference = directory / "reference"
    reference.mkdir(parents=True, exist_ok=True)
    manifest_path = directory / "manifest.json"
    if manifest_path.exists():
        manifest = json.loads(manifest_path.read_text())
        if manifest.get("config_sha256") != config_hash:
            raise ValueError("Existing suite configuration/provenance differs; choose a new output directory")
    else:
        manifest = {"schema_version": 1, "config": config, "config_sha256": config_hash,
                    "requested_presets": [], "completed_presets": [], "cases": [],
                    "limitations": [
                        "Instruction emulation is the chosen comparison target, not old macOS/DAW execution.",
                        "Current ARM libSystem cosf/expf/powf boundary is shared by comparison.",
                        "Dry mono voice/control/render only; effects and 4x oversampling are excluded.",
                        "Explicit sample-boundary note events are the test contract, not the old host MIDI queue's lookahead quantization.",
                    ]}
    manifest["requested_presets"] = sorted(set(manifest["requested_presets"]) | set(selected))
    write_json(manifest_path, manifest)
    oracle = None
    started = time.monotonic()
    for index in selected:
        case_path = reference / f"preset-{index:03d}.json"
        wav_path = reference / f"preset-{index:03d}.wav"
        if args.resume and case_path.exists() and wav_path.exists():
            case = json.loads(case_path.read_text())
            wav = wav_path.read_bytes()
            valid = (case.get("config_sha256") == config_hash and case.get("index") == index
                     and case.get("wav_sha256") == sha(wav)
                     and case.get("payload_sha256") == sha(wav_payload(wav)))
            if not valid:
                raise ValueError(f"Resume artifact failed verification for preset {index}")
            print(json.dumps({"preset": index, "status": "verified-resume", "file": str(wav_path)}), flush=True)
        else:
            if oracle is None:
                oracle = VoiceOracle(args.binary)
            program = factory["programs"][index]
            parameters = program["values"].copy()
            parameters[37] = 0.0
            before = time.monotonic()
            payload, stats = render_case(oracle, parameters, config)
            wav = float_wav(payload, args.rate)
            temporary = wav_path.with_suffix(".wav.tmp")
            temporary.write_bytes(wav)
            temporary.replace(wav_path)
            case = {"index": index, "name": program["name"], "config_sha256": config_hash,
                    "parameters_float32_bits": [f"{struct.unpack('<I', struct.pack('<f', value))[0]:08x}" for value in parameters],
                    "parameters_sha256": sha(struct.pack("<38f", *parameters)),
                    "wav": str(wav_path.relative_to(directory)), "wav_sha256": sha(wav),
                    "payload_sha256": sha(payload), "render_seconds": time.monotonic() - before,
                    **stats}
            write_json(case_path, case)
            print(json.dumps({"preset": index, "name": program["name"], "status": "rendered",
                              "seconds": round(case["render_seconds"], 3), "peak": stats["peak"],
                              "nonzero_samples": stats["nonzero_samples"]}), flush=True)
        manifest["cases"] = sorted([entry for entry in manifest["cases"] if entry["index"] != index] + [case],
                                   key=lambda entry: entry["index"])
        manifest["completed_presets"] = [entry["index"] for entry in manifest["cases"]]
        manifest["complete"] = manifest["completed_presets"] == list(range(128))
        write_json(manifest_path, manifest)
    return {"output": str(directory), "completed_presets": len(manifest["completed_presets"]),
            "complete_128": manifest["complete"], "elapsed_seconds": round(time.monotonic() - started, 3),
            "config_sha256": config_hash}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=DEFAULT_BINARY)
    parser.add_argument("--output", type=Path, default=ROOT / "analysis/recovered/preset-renders")
    parser.add_argument("--presets", default="all", help="all, comma-separated zero-based indexes or inclusive ranges")
    parser.add_argument("--rate", type=int, default=44100)
    parser.add_argument("--block-size", type=int, default=512)
    parser.add_argument("--hold-seconds", type=float, default=2.0)
    parser.add_argument("--release-seconds", type=float, default=4.0)
    parser.add_argument("--resume", action=argparse.BooleanOptionalAction, default=True)
    args = parser.parse_args()
    print(json.dumps(suite(args), indent=2), flush=True)


if __name__ == "__main__":
    main()
