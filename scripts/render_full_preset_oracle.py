#!/usr/bin/env python3
"""Export separate stereo factory references from original i386 full DSP instructions.

Preserves all 38 factory parameters, including their FX mix and oversampling
setting. "Full" means the complete selected signal path, not a forced 100% wet
mix. These are emulator references with current ARM libSystem math imports, not
recordings from an old macOS host and not a native-engine equivalence claim.
"""
from __future__ import annotations

import argparse
import array
from collections import Counter
import json
import math
from pathlib import Path
import platform
import struct
import sys
import time

import unicorn
from full_dsp_oracle import FullDSPOracle
from render_preset_oracle import OriginalScheduler, canonical, parse_presets, sha, write_json
from voice_oracle import ROOT, f32
from x86_oracle import DEFAULT_BINARY, DSP_FPCW


PROTOCOL = "plasticz-original-full-factory-c1-v1"
DEPENDENCIES = ("render_full_preset_oracle.py", "full_dsp_oracle.py", "render_preset_oracle.py",
                "voice_oracle.py", "x86_oracle.py")
LOADED_HASHES = {name: sha((ROOT / "scripts" / name).read_bytes()) for name in DEPENDENCIES}


def verify_dependencies(expected):
    for name, digest in expected.items():
        if sha((ROOT / "scripts" / name).read_bytes()) != digest:
            raise RuntimeError(f"Oracle dependency changed during this run: {name}; restart in a new provenance directory")


def interleave(left: bytes, right: bytes, frames: int) -> bytes:
    if len(left) != frames * 4 or len(right) != frames * 4:
        raise RuntimeError("Full DSP oracle returned invalid stereo lengths")
    # Move uint32 words without converting float values or changing their bits.
    lwords, rwords = array.array("I"), array.array("I")
    lwords.frombytes(left)
    rwords.frombytes(right)
    output = array.array("I", [0]) * (frames * 2)
    output[0::2], output[1::2] = lwords, rwords
    return output.tobytes()


class StereoSchedulerAdapter:
    """Keep OriginalScheduler unchanged; only adapt its render-byte contract."""
    def __init__(self, oracle):
        self.oracle = oracle

    def read_float(self, offset):
        return self.oracle.read_float(offset)

    def write_float(self, offset, value):
        self.oracle.write_float(offset, value)

    def control_tick(self):
        self.oracle.control_tick()

    def render(self, frames):
        left, right = self.oracle.render(frames)
        return interleave(left, right, frames)


def float_wav(payload: bytes, rate: int) -> bytes:
    if len(payload) % 8 or rate <= 0:
        raise ValueError("Expected whole stereo float32 frames and positive sample rate")
    fmt = struct.pack("<HHIIHH", 3, 2, rate, rate * 8, 8, 32)
    return (b"RIFF" + struct.pack("<I", 48 + len(payload)) + b"WAVE"
            + b"fmt " + struct.pack("<I", 16) + fmt
            + b"fact" + struct.pack("<II", 4, len(payload) // 8)
            + b"data" + struct.pack("<I", len(payload)) + payload)


def wav_payload(wav: bytes, expected_rate: int | None = None) -> bytes:
    if len(wav) < 12 or wav[:4] != b"RIFF" or wav[8:12] != b"WAVE":
        raise ValueError("Expected RIFF WAV")
    if struct.unpack_from("<I", wav, 4)[0] != len(wav) - 8:
        raise ValueError("Inconsistent RIFF size")
    cursor, format_seen, fact, payload = 12, False, None, None
    while cursor < len(wav):
        if cursor + 8 > len(wav):
            raise ValueError("Truncated WAV chunk header")
        tag, size = wav[cursor:cursor + 4], struct.unpack_from("<I", wav, cursor + 4)[0]
        start, end = cursor + 8, cursor + 8 + size
        if end > len(wav):
            raise ValueError("WAV chunk exceeds file")
        if tag == b"fmt ":
            if size != 16 or format_seen:
                raise ValueError("Unexpected WAV format chunk")
            encoding, channels, rate, byte_rate, align, bits = struct.unpack_from("<HHIIHH", wav, start)
            if (encoding, channels, align, bits) != (3, 2, 8, 32) or byte_rate != rate * 8 or rate <= 0:
                raise ValueError("Expected stereo IEEE float32 WAV")
            if expected_rate is not None and rate != expected_rate:
                raise ValueError("WAV sample rate differs from suite")
            format_seen = True
        elif tag == b"fact":
            if size != 4 or fact is not None:
                raise ValueError("Unexpected WAV fact chunk")
            fact = struct.unpack_from("<I", wav, start)[0]
        elif tag == b"data":
            if not format_seen or payload is not None or size % 8:
                raise ValueError("Invalid stereo WAV data chunk")
            payload = wav[start:end]
        cursor = end + (size & 1)
    if payload is None or fact != len(payload) // 8 or cursor != len(wav):
        raise ValueError("Missing or inconsistent WAV frame count")
    return payload


def audio_stats(payload: bytes) -> dict:
    if len(payload) % 8:
        raise RuntimeError("Audio is not whole stereo float32 frames")
    values = array.array("f")
    values.frombytes(payload)
    if sys.byteorder != "little":
        values.byteswap()
    if not all(math.isfinite(value) for value in values):
        raise RuntimeError("Non-finite original full DSP output")
    channels = []
    for samples in (values[0::2], values[1::2]):
        channels.append({"peak": max(map(abs, samples), default=0.0),
                         "rms": math.sqrt(sum(float(x) * x for x in samples) / max(len(samples), 1)),
                         "mean": sum(samples) / max(len(samples), 1),
                         "nonzero_samples": sum(x != 0.0 for x in samples)})
    return {"frames": len(values) // 2, "channels": channels,
            "peak": max(channel["peak"] for channel in channels),
            "nonzero_samples": sum(channel["nonzero_samples"] for channel in channels),
            "stereo_different_frames": sum(a != b for a, b in zip(values[0::2], values[1::2]))}


def render_case(oracle, parameters: list[float], config: dict) -> tuple[bytes, dict]:
    imports_before = Counter(getattr(oracle, "import_calls", {}))
    extended_before = Counter(getattr(oracle, "extended_calls", {}))
    oracle.initialize(parameters, config["sample_rate"])
    scheduler = OriginalScheduler(StereoSchedulerAdapter(oracle), config["sample_rate"])
    preroll = scheduler.render(config["preroll_frames"])
    if len(preroll) != config["preroll_frames"] * 8:
        raise RuntimeError("Unexpected full DSP preroll length")
    preroll_stats = audio_stats(preroll)  # Original output stages may have a bias with no held note.
    countdown_at_note_on = oracle.read_float(0x406A44)
    oracle.note_on(config["note"], config["velocity"], config["channel"])
    result, frame, released, host_blocks = bytearray(), 0, False, 0
    total = config["hold_frames"] + config["release_frames"]
    while frame < total:
        block_end = min(total, frame + config["block_size"])
        host_blocks += 1
        while frame < block_end:
            if frame == config["hold_frames"] and not released:
                oracle.note_off(config["note"], config["channel"])
                released = True
            boundary = block_end if released else min(block_end, config["hold_frames"])
            result.extend(scheduler.render(boundary - frame))
            frame = boundary
    if not released:
        oracle.note_off(config["note"], config["channel"])
    if len(result) != total * 8:
        raise RuntimeError("Unexpected full DSP render length")
    stats = audio_stats(result)
    stats.update({"host_blocks": host_blocks,
                  "control_ticks_including_preroll": scheduler.ticks,
                  "original_render_calls_including_preroll": scheduler.render_calls,
                  "countdown_at_note_on": countdown_at_note_on,
                  "countdown_at_end": oracle.read_float(0x406A44),
                  "preroll": {**preroll_stats, "payload_sha256": sha(preroll)},
                  "tail_last_second": audio_stats(result[-min(total, config["sample_rate"]) * 8:]),
                  "base_import_calls": dict(Counter(getattr(oracle, "import_calls", {})) - imports_before),
                  "full_import_calls": dict(Counter(getattr(oracle, "extended_calls", {})) - extended_before)})
    return bytes(result), stats


def verify_unknown_import_fails_closed(oracle):
    unknown = next(address for address in range(0x69000, 0x69681, 5)
                   if address not in oracle.imports and address not in oracle.extra_imports)
    try:
        oracle.invoke(unknown, b"")
    except RuntimeError as error:
        if "Unimplemented original import" in str(error):
            return {"verified": True, "address": hex(unknown), "message": str(error)}
        raise
    raise RuntimeError("Unknown import did not fail closed")


def verify_resume(case: dict, wav: bytes, config: dict, config_hash: str, program: dict):
    payload = wav_payload(wav, config["sample_rate"])
    parameter_bytes = struct.pack("<38f", *program["values"])
    expected_frames = config["hold_frames"] + config["release_frames"]
    if not (case.get("config_sha256") == config_hash and case.get("index") == program["index"]
            and case.get("wav_sha256") == sha(wav) and case.get("payload_sha256") == sha(payload)
            and case.get("parameters_sha256") == sha(parameter_bytes)
            and case.get("frames") == expected_frames and len(payload) == expected_frames * 8):
        raise ValueError(f"Resume artifact failed verification for preset {program['index']}")
    audio_stats(payload)  # Hash-valid sidecars cannot authorize NaN/Inf payloads.


def suite(args):
    if not 8000 <= args.rate <= 384000 or not 1 <= args.block_size <= 32768:
        raise ValueError("Unsupported sample rate or host block size")
    if not all(math.isfinite(x) and x >= 0 for x in (args.hold_seconds, args.release_seconds)):
        raise ValueError("Durations must be finite and nonnegative")
    hold, release = round(args.hold_seconds * args.rate), round(args.release_seconds * args.rate)
    if hold < 1 or hold + release > args.rate * 300:
        raise ValueError("Hold must contain frames; total duration must be <=300 seconds")
    selected = parse_presets(args.presets)
    factory_bytes = (ROOT / "analysis/recovered/factory-presets.json").read_bytes()
    factory = json.loads(factory_bytes)
    verify_dependencies(LOADED_HASHES)
    config = {"protocol": PROTOCOL, "sample_rate": args.rate, "block_size": args.block_size,
              "hold_frames": hold, "release_frames": release, "preroll_frames": 512,
              "note": 24, "velocity": f32(100.0 / 127.0), "channel": 0, "channels": 2,
              "tempo_bpm": 120.0, "parameter_overrides": {},
              "wav_format": "IEEE float32 little-endian stereo interleaved (format tag 3)",
              "effects": "Original selected effects/output dispatcher 0x10e4a, factory FX mix and oversampling preserved",
              "event_timing": "Discard 512 no-note frames through persistent original scheduler; note_on before emitted frame0, note_off before emitted hold_frames; host blocks anchored at frame0 and split exactly at note-off.",
              "scheduler": "Unmodified OriginalScheduler from render_preset_oracle.py; original +0x406a44 float32 countdown and boundary control ticks; stereo byte adapter only.",
              "preroll_policy": "Measure and discard; no assumption of zero output from original output/effect stages",
              "source_sha256": sha(DEFAULT_BINARY.read_bytes()), "factory_json_sha256": sha(factory_bytes),
              "dependency_sha256": LOADED_HASHES,
              "full_helper_sha256": LOADED_HASHES["full_dsp_oracle.py"],
              "x87_control_word": hex(DSP_FPCW), "sse_mxcsr": "0x1f80",
              "math_boundary": "Current ARM /usr/lib/libSystem.B.dylib: cosf, expf, powf, sinf, cos, pow, sin, fmod, floor, fmax. Original allocation/memory imports emulated by bounded per-fixture arena; unknown imports fail closed.",
              "environment": {"system": platform.system(), "macos": platform.mac_ver()[0],
                              "kernel": platform.release(), "machine": platform.machine(),
                              "python": sys.version, "unicorn": unicorn.__version__},
              "canonical_duration": args.rate == 44100 and args.block_size == 512 and hold == 88200 and release == 176400}
    config_hash = sha(canonical(config))
    directory = args.output
    if directory.resolve() == (ROOT / "analysis/recovered/preset-renders").resolve():
        raise ValueError("Full DSP references must not overwrite the dry suite")
    reference = directory / "reference"
    reference.mkdir(parents=True, exist_ok=True)
    manifest_path = directory / "manifest.json"
    if manifest_path.exists():
        manifest = json.loads(manifest_path.read_text())
        if manifest.get("config_sha256") != config_hash:
            raise ValueError("Existing suite configuration/provenance differs; choose a new output directory")
    else:
        manifest = {"schema_version": 1, "config": config, "config_sha256": config_hash,
                    "requested_presets": [], "completed_presets": [], "cases": [], "failures": {},
                    "native_comparison": "Not performed; current native engine excludes original FX",
                    "limitations": ["Isolated instruction emulator reference, not legacy macOS/DAW execution.",
                                    "Current ARM libSystem math may differ from historical x86 math libraries.",
                                    "Explicit sample-boundary MIDI event contract, not old host queue timing.",
                                    "Helper initialization is separately audited; its exact source hash is pinned.",
                                    "Factory FX settings are preserved; full signal path does not force 100% wet mix."]}
    manifest["requested_presets"] = sorted(set(manifest["requested_presets"]) | set(selected))
    write_json(manifest_path, manifest)
    oracle, started = None, time.monotonic()
    for index in selected:
        verify_dependencies(LOADED_HASHES)
        program = factory["programs"][index]
        if program["index"] != index or struct.pack("<38f", *program["values"]) != struct.pack("<38I", *(int(x, 16) for x in program["float32_bits_hex"])):
            raise ValueError("Factory parameter order or float words differ")
        case_path, wav_path = reference / f"preset-{index:03d}.json", reference / f"preset-{index:03d}.wav"
        try:
            if args.resume and case_path.exists() and wav_path.exists():
                case, wav = json.loads(case_path.read_text()), wav_path.read_bytes()
                verify_resume(case, wav, config, config_hash, program)
                if args.repeat_check and not case.get("deterministic_repeat", {}).get("verified"):
                    raise ValueError("Resume case lacks requested determinism check; rerun with --no-resume")
                print(json.dumps({"preset": index, "status": "verified-resume"}), flush=True)
            else:
                if oracle is None:
                    oracle = FullDSPOracle()
                    if oracle.sha256 != config["source_sha256"]:
                        raise RuntimeError("Loaded original binary differs from pinned source hash")
                    manifest["unknown_import_guard"] = verify_unknown_import_fails_closed(oracle)
                before = time.monotonic()
                payload, stats = render_case(oracle, program["values"].copy(), config)
                repeated = {"verified": False, "reason": "Not requested for this case"}
                if args.repeat_check:
                    again, again_stats = render_case(oracle, program["values"].copy(), config)
                    if payload != again or stats["preroll"]["payload_sha256"] != again_stats["preroll"]["payload_sha256"]:
                        raise RuntimeError("Reinitialization/repeated render changed original stereo bytes")
                    repeated = {"verified": True, "payload_sha256": sha(again), "preroll_sha256": again_stats["preroll"]["payload_sha256"]}
                verify_dependencies(LOADED_HASHES)
                wav = float_wav(payload, args.rate)
                temporary = wav_path.with_suffix(".wav.tmp")
                temporary.write_bytes(wav)
                temporary.replace(wav_path)
                case = {"index": index, "name": program["name"], "config_sha256": config_hash,
                        "parameters_float32_bits": program["float32_bits_hex"],
                        "parameters_sha256": sha(struct.pack("<38f", *program["values"])),
                        "wav": str(wav_path.relative_to(directory)), "wav_sha256": sha(wav),
                        "payload_sha256": sha(payload), "render_seconds": time.monotonic() - before,
                        "deterministic_repeat": repeated, **stats}
                write_json(case_path, case)
                print(json.dumps({"preset": index, "name": program["name"], "status": "rendered",
                                  "seconds": round(case["render_seconds"], 3), "peak": stats["peak"],
                                  "preroll_peak": stats["preroll"]["peak"], "repeat_verified": repeated["verified"]}), flush=True)
            manifest["failures"].pop(str(index), None)
            manifest["cases"] = sorted([case for case in manifest["cases"] if case["index"] != index] + [case], key=lambda case: case["index"])
            manifest["completed_presets"] = [case["index"] for case in manifest["cases"]]
            manifest["complete_128"] = manifest["completed_presets"] == list(range(128))
            manifest["full_protocol_complete"] = manifest["complete_128"] and config["canonical_duration"]
            write_json(manifest_path, manifest)
        except Exception as error:
            manifest["failures"][str(index)] = {"type": type(error).__name__, "error": str(error)}
            write_json(manifest_path, manifest)
            raise
    summary = {"output": str(directory), "completed_presets": len(manifest["completed_presets"]),
               "complete_128": manifest.get("complete_128", False),
               "full_protocol_complete": manifest.get("full_protocol_complete", False),
               "deterministic_repeat_count": sum(case["deterministic_repeat"]["verified"] for case in manifest["cases"]),
               "nonzero_preroll_count": sum(case["preroll"]["nonzero_samples"] > 0 for case in manifest["cases"]),
               "peak": max((case["peak"] for case in manifest["cases"]), default=0),
               "elapsed_seconds": round(time.monotonic() - started, 3), "config_sha256": config_hash,
               "native_comparison": "Not performed"}
    write_json(directory / "summary.json", summary)
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "analysis/recovered/preset-renders-full")
    parser.add_argument("--presets", default="all")
    parser.add_argument("--rate", type=int, default=44100)
    parser.add_argument("--block-size", type=int, default=512)
    parser.add_argument("--hold-seconds", type=float, default=2.0)
    parser.add_argument("--release-seconds", type=float, default=4.0)
    parser.add_argument("--repeat-check", action="store_true")
    parser.add_argument("--resume", action=argparse.BooleanOptionalAction, default=True)
    print(json.dumps(suite(parser.parse_args()), indent=2), flush=True)


if __name__ == "__main__":
    main()
