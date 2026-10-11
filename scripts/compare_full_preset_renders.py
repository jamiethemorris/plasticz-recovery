#!/usr/bin/env python3
"""Compare the current native dry preview with full original stereo renders.

This is deliberately separate from the dry regression. The original retains its
factory effects and 4x settings, so differences are expected until those paths
are implemented. Exit 1 means audio differences; exit 2 means invalid evidence.
No normalization, alignment search, tolerance, or processing is applied.
"""
from __future__ import annotations

import argparse
import array
import csv
import hashlib
import json
import math
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def read_wav(path: Path, channels: int, rate: int, frames: int):
    wav = path.read_bytes()
    if len(wav) < 44 or wav[:4] != b"RIFF" or wav[8:12] != b"WAVE":
        raise ValueError(f"Expected RIFF WAV: {path}")
    if struct.unpack_from("<I", wav, 4)[0] + 8 != len(wav):
        raise ValueError(f"RIFF length mismatch: {path}")
    offset, payload, format_seen = 12, None, False
    while offset < len(wav):
        if offset + 8 > len(wav):
            raise ValueError("Truncated WAV chunk header")
        tag, size = wav[offset:offset + 4], struct.unpack_from("<I", wav, offset + 4)[0]
        begin, end = offset + 8, offset + 8 + size
        if end > len(wav):
            raise ValueError("WAV chunk exceeds file length")
        if tag == b"fmt ":
            if format_seen or size < 16:
                raise ValueError("Duplicate or truncated WAV format")
            actual = struct.unpack_from("<HHIIHH", wav, begin)
            expected = (3, channels, rate, rate * channels * 4, channels * 4, 32)
            if actual != expected:
                raise ValueError(f"Unexpected WAV format: {actual} != {expected}")
            format_seen = True
        elif tag == b"data":
            if payload is not None or size != frames * channels * 4:
                raise ValueError(f"Unexpected WAV frame count: {path}")
            payload = wav[begin:end]
        offset = end + (size & 1)
    if not format_seen or payload is None or offset != len(wav):
        raise ValueError(f"Missing WAV format/data: {path}")
    values, words = array.array("f"), array.array("I")
    values.frombytes(payload)
    words.frombytes(payload)
    if sys.byteorder != "little":
        values.byteswap()
        words.byteswap()
    if not all(math.isfinite(value) for value in values):
        raise ValueError(f"Non-finite audio: {path}")
    return values, words, {"wav_sha256": sha(wav), "payload_sha256": sha(payload)}


def write_stereo(path: Path, samples: array.array, rate: int):
    if sys.byteorder != "little":
        samples.byteswap()
    payload = samples.tobytes()
    content = (b"RIFF" + struct.pack("<I", 48 + len(payload)) + b"WAVEfmt "
               + struct.pack("<IHHIIHH", 16, 3, 2, rate, rate * 8, 8, 32)
               + b"fact" + struct.pack("<II", 4, len(payload) // 8)
               + b"data" + struct.pack("<I", len(payload)) + payload)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(content)


def verify_timing(original: dict, native: dict):
    checks = {
        "sample_rate": native["sample_rate"],
        "block_size": native["host_block"],
        "preroll_frames": native["silent_preroll_frames"],
        "note": native["note"],
        "hold_frames": round(native["held_seconds"] * native["sample_rate"]),
        "release_frames": round(native["tail_seconds"] * native["sample_rate"]),
    }
    for key, expected in checks.items():
        if original.get(key) != expected:
            raise ValueError(f"Timing/configuration mismatch: {key}: {original.get(key)} != {expected}")
    if struct.pack("<f", original["velocity"]) != struct.pack("<f", native["velocity_midi"] / 127):
        raise ValueError("Velocity differs")
    if original.get("channels") != 2 or original.get("parameter_overrides") != {}:
        raise ValueError("Expected full original stereo with unchanged factory parameters")
    if not original.get("protocol", "").startswith("plasticz-original-full-factory-c1-"):
        raise ValueError("Unexpected full-reference protocol")
    if native.get("effects") is not False or native.get("oversampling") is not False:
        raise ValueError("This comparator labels the current dry/non-4x preview only")


def compare_case(reference, reference_words, native, native_words):
    frames = len(native)
    difference = array.array("f")
    mismatched_frames = mismatched_samples = 0
    first_frame, first_channel = -1, -1
    maximum = squared = 0.0
    reference_peak = [0.0, 0.0]
    reference_squared = [0.0, 0.0]
    native_peak = native_squared = stereo_peak = 0.0
    for frame in range(frames):
        actual, actual_bits = native[frame], native_words[frame]
        native_peak = max(native_peak, abs(actual))
        native_squared += actual * actual
        changed = False
        stereo_peak = max(stereo_peak, abs(reference[2 * frame] - reference[2 * frame + 1]))
        for channel in (0, 1):
            index = frame * 2 + channel
            wanted = reference[index]
            if reference_words[index] != actual_bits:
                mismatched_samples += 1
                changed = True
                if first_frame < 0:
                    first_frame, first_channel = frame, channel
            error = float(actual) - float(wanted)
            difference.append(error)
            maximum = max(maximum, abs(error))
            squared += error * error
            reference_peak[channel] = max(reference_peak[channel], abs(wanted))
            reference_squared[channel] += wanted * wanted
        mismatched_frames += changed
    return {"frames": frames, "channel_samples": frames * 2,
            "bit_mismatched_channel_samples": mismatched_samples,
            "mismatched_frames": mismatched_frames,
            "first_mismatch_frame": first_frame, "first_mismatch_channel": first_channel,
            "max_abs_error": maximum, "rms_error": math.sqrt(squared / (frames * 2)),
            "square_error_sum": squared,
            "reference_peak_left": reference_peak[0], "reference_peak_right": reference_peak[1],
            "reference_rms_left": math.sqrt(reference_squared[0] / frames),
            "reference_rms_right": math.sqrt(reference_squared[1] / frames),
            "reference_stereo_difference_peak": stereo_peak,
            "native_peak": native_peak, "native_rms": math.sqrt(native_squared / frames)}, difference


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference-suite", type=Path, default=ROOT / "analysis/recovered/preset-renders-full")
    parser.add_argument("--native", type=Path, default=ROOT / "analysis/recovered/preset-renders/native")
    parser.add_argument("--output", type=Path, default=ROOT / "analysis/recovered/preset-renders-full/preview-comparison")
    parser.add_argument("--allow-incomplete", action="store_true", help="Compare completed original cases and mark the report partial")
    args = parser.parse_args()
    manifest_path = args.reference_suite / "manifest.json"
    original_bytes = manifest_path.read_bytes()
    original = json.loads(original_bytes)
    canonical_config = json.dumps(original["config"], sort_keys=True, separators=(",", ":"), allow_nan=False).encode()
    if sha(canonical_config) != original["config_sha256"]:
        raise ValueError("Original configuration hash differs")
    native_manifest_path = args.native / "native-manifest.json"
    native_bytes = native_manifest_path.read_bytes()
    native = json.loads(native_bytes)
    native_provenance_bytes = (args.native / "provenance.json").read_bytes()
    native_provenance = json.loads(native_provenance_bytes)
    if sha(native_bytes) != native_provenance["reports_sha256"]["native/native-manifest.json"]:
        raise ValueError("Native manifest differs from the verified dry provenance")
    verify_timing(original["config"], native)
    if native["first_preset"] != 0 or native["preset_count"] != 128:
        raise ValueError("Native manifest must describe the complete 128-preset render")
    completed = original["completed_presets"]
    if len(completed) != len(set(completed)) or any(index not in range(128) for index in completed):
        raise ValueError("Duplicate or invalid completed preset indexes")
    if not completed or (not args.allow_incomplete and sorted(completed) != list(range(128))):
        raise ValueError("Full original reference suite is not complete; use --allow-incomplete for a labeled partial report")
    if not args.allow_incomplete and original.get("full_protocol_complete") is not True:
        raise ValueError("Original manifest has not verified the complete full protocol")
    cases = {case["index"]: case for case in original["cases"]}
    if len(cases) != len(original["cases"]) or sorted(cases) != sorted(completed):
        raise ValueError("Original cases disagree with completed preset indexes")
    reference_dir = (args.reference_suite / "reference").resolve()
    if args.output.resolve() in (reference_dir, args.native.resolve()):
        raise ValueError("Comparison output cannot overwrite reference/native directories")
    args.output.mkdir(parents=True, exist_ok=True)
    frame_count = original["config"]["hold_frames"] + original["config"]["release_frames"]
    rate = original["config"]["sample_rate"]
    rows = []
    for index in sorted(completed):
        name = f"preset-{index:03d}.wav"
        if cases[index].get("config_sha256") != original["config_sha256"]:
            raise ValueError(f"Original case configuration differs for preset {index}")
        reference, reference_words, ref_hash = read_wav(reference_dir / name, 2, rate, frame_count)
        if any(cases[index].get(key) != value for key, value in ref_hash.items()):
            raise ValueError(f"Original evidence hash changed for preset {index}")
        values, words, native_hash = read_wav(args.native / name, 1, rate, frame_count)
        if native_hash["wav_sha256"] != native_provenance["native_wav_sha256"][name]:
            raise ValueError(f"Native audio differs from the verified dry provenance for preset {index}")
        result, difference = compare_case(reference, reference_words, values, words)
        difference_path = args.output / "difference" / name
        write_stereo(difference_path, difference, rate)
        rows.append({"preset": index, "name": cases[index]["name"], **result,
                     "reference_hashes": ref_hash, "native_hashes": native_hash,
                     "difference_wav_sha256": sha(difference_path.read_bytes())})
        print(f"Compared full original preset {index:03d}: {result['bit_mismatched_channel_samples']} differing channel samples", flush=True)
    csv_columns = [key for key in rows[0] if key not in ("reference_hashes", "native_hashes", "square_error_sum")]
    with (args.output / "comparison.csv").open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=csv_columns, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)
    samples = sum(row["channel_samples"] for row in rows)
    mismatches = sum(row["bit_mismatched_channel_samples"] for row in rows)
    mismatching_presets = [row["preset"] for row in rows if row["bit_mismatched_channel_samples"]]
    summary = {
        "comparison": "current native dry mono duplicated to stereo versus full original factory stereo",
        "complete_128": len(rows) == 128, "presets_compared": len(rows), "channel_samples": samples,
        "bit_mismatched_channel_samples": mismatches, "mismatching_presets": mismatching_presets,
        "max_abs_error": max(row["max_abs_error"] for row in rows),
        "rms_error": math.sqrt(sum(row["square_error_sum"] for row in rows) / samples),
        "exact_audio_match": mismatches == 0,
        "expected_limitations": ["Current native effects are not implemented", "Current native original 4x path is not implemented",
                                 "Original output/effect processing is included only on the reference side"],
        "interpretation": "This comparison measures missing full processing; it does not invalidate the separately verified dry path.",
        "reference_config_sha256": original["config_sha256"], "reference_manifest_sha256": sha(original_bytes),
        "native_manifest_sha256": sha(native_bytes), "script_sha256": sha(Path(__file__).read_bytes()),
        "native_provenance_sha256": sha(native_provenance_bytes),
        "reference_config": original["config"], "native_config": native, "cases": rows,
    }
    (args.output / "summary.json").write_text(json.dumps(summary, indent=2, allow_nan=False) + "\n")
    print(json.dumps({key: summary[key] for key in ("complete_128", "presets_compared", "channel_samples",
                    "bit_mismatched_channel_samples", "max_abs_error", "rms_error", "exact_audio_match")}, indent=2))
    return 1 if mismatches else 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ValueError, OSError, KeyError) as error:
        print(f"Comparison evidence error: {error}", file=sys.stderr)
        raise SystemExit(2)
