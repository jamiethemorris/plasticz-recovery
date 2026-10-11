#!/usr/bin/env python3
"""Merge completed, disjoint full-reference shards after validating every artifact.

No DSP is executed. All 128 unique canonical-duration cases and identical frozen
provenance are required before writing the consolidated manifest. Original shard
manifests are copied into the output's provenance directory before replacement.
"""
import argparse
import json
from pathlib import Path
import shutil

from render_full_preset_oracle import PROTOCOL, ROOT, canonical, f32, sha, verify_dependencies, verify_resume, write_json


def merge(sources: list[Path], output: Path):
    if len({source.resolve() for source in sources}) != len(sources):
        raise ValueError("Source directories must be distinct")
    factory_bytes = (ROOT / "analysis/recovered/factory-presets.json").read_bytes()
    factory = json.loads(factory_bytes)
    records, snapshots, common_config, common_hash = {}, [], None, None
    for source in sources:
        manifest_path = source / "manifest.json"
        snapshot = manifest_path.read_bytes()
        manifest = json.loads(snapshot)
        config, digest = manifest["config"], manifest["config_sha256"]
        if digest != sha(canonical(config)) or config.get("protocol") != PROTOCOL or not config.get("canonical_duration"):
            raise ValueError(f"Source lacks verified canonical full-protocol configuration: {source}")
        expected = {"sample_rate": 44100, "block_size": 512, "hold_frames": 88200,
                    "release_frames": 176400, "preroll_frames": 512, "note": 24,
                    "velocity": f32(100 / 127), "channel": 0, "channels": 2,
                    "parameter_overrides": {}, "factory_json_sha256": sha(factory_bytes)}
        if any(config.get(key) != value for key, value in expected.items()):
            raise ValueError(f"Source differs from the canonical protocol or recovered factory data: {source}")
        if common_config is None:
            common_config, common_hash = config, digest
        elif digest != common_hash or config != common_config:
            raise ValueError("Shard configurations/provenance differ")
        if manifest.get("failures") or not manifest.get("unknown_import_guard", {}).get("verified"):
            raise ValueError(f"Source has failures or lacks its unknown-import guard: {source}")
        listed = [case["index"] for case in manifest["cases"]]
        if listed != manifest.get("completed_presets") or len(set(listed)) != len(listed):
            raise ValueError(f"Source completed-case index mismatch: {source}")
        for entry in manifest["cases"]:
            index = entry["index"]
            if type(index) is not int or not 0 <= index < 128 or index in records:
                raise ValueError("Shards must contain disjoint valid preset indexes")
            case_path = source / "reference" / f"preset-{index:03d}.json"
            wav_path = source / "reference" / f"preset-{index:03d}.wav"
            case = json.loads(case_path.read_text())
            if case != entry or case.get("wav") != f"reference/preset-{index:03d}.wav":
                raise ValueError(f"Shard manifest and sidecar differ at preset {index}")
            if case.get("parameters_float32_bits") != factory["programs"][index]["float32_bits_hex"]:
                raise ValueError(f"Factory parameter words differ at preset {index}")
            verify_resume(case, wav_path.read_bytes(), config, digest, factory["programs"][index])
            records[index] = (case, case_path, wav_path)
        snapshots.append((source, snapshot, listed, manifest.get("unknown_import_guard")))
    if sorted(records) != list(range(128)):
        raise ValueError(f"Expected every unique preset 0..127; found {len(records)}")
    verify_dependencies(common_config["dependency_sha256"])
    # Validate all existing destinations before making any changes.
    reference = output / "reference"
    for index, (case, case_path, wav_path) in records.items():
        destination = reference / wav_path.name
        if destination.exists() and sha(destination.read_bytes()) != case["wav_sha256"]:
            raise ValueError(f"Existing destination WAV differs for preset {index}")
        destination = reference / case_path.name
        if destination.exists() and json.loads(destination.read_text()) != case:
            raise ValueError(f"Existing destination sidecar differs for preset {index}")
    reference.mkdir(parents=True, exist_ok=True)
    provenance = output / "provenance"
    provenance.mkdir(parents=True, exist_ok=True)
    origins = []
    for number, (source, snapshot, listed, guard) in enumerate(snapshots):
        snapshot_path = provenance / f"shard-{number:02d}.manifest.json"
        if snapshot_path.exists() and snapshot_path.read_bytes() != snapshot:
            raise ValueError("Existing shard provenance snapshot differs")
        snapshot_path.write_bytes(snapshot)
        origins.append({"directory": str(source), "original_manifest_sha256": sha(snapshot),
                        "snapshot": str(snapshot_path.relative_to(output)), "presets": listed,
                        "unknown_import_guard": guard})
    for index, (case, case_path, wav_path) in sorted(records.items()):
        for source in (wav_path, case_path):
            target = reference / source.name
            if source.resolve() == target.resolve():
                continue
            temporary = target.with_suffix(target.suffix + ".tmp")
            shutil.copyfile(source, temporary)
            if source == wav_path and sha(temporary.read_bytes()) != case["wav_sha256"]:
                temporary.unlink()
                raise ValueError(f"Source WAV changed while copying preset {index}")
            if source == case_path and json.loads(temporary.read_text()) != case:
                temporary.unlink()
                raise ValueError(f"Source sidecar changed while copying preset {index}")
            temporary.replace(target)
    cases = [records[index][0] for index in range(128)]
    manifest = {"schema_version": 1, "config": common_config, "config_sha256": common_hash,
                "requested_presets": list(range(128)), "completed_presets": list(range(128)),
                "cases": cases, "failures": {}, "complete_128": True, "full_protocol_complete": True,
                "unknown_import_guard": {"verified": True, "all_shards": True}, "merged_from": origins,
                "native_comparison": "Not performed by reference exporter",
                "limitations": ["Isolated original i386 instruction execution with pinned ARM libSystem math, not a legacy host recording.",
                                "Short all-preset repeat coverage is separate from these canonical-duration renders.",
                                "Successful reference export does not establish native-engine equivalence."]}
    write_json(output / "manifest.json", manifest)
    summary = {"output": str(output), "completed_presets": 128, "complete_128": True,
               "full_protocol_complete": True, "config_sha256": common_hash,
               "frames_per_preset": 264600, "channels": 2, "format": "IEEE float32 stereo LE",
               "source_shards": len(sources), "verified_unique_indexes": 128,
               "verified_wav_hashes": 128, "verified_payload_hashes": 128, "verified_parameter_hashes": 128,
               "deterministic_repeat_count": sum(case["deterministic_repeat"]["verified"] for case in cases),
               "nonzero_preroll_count": sum(case["preroll"]["nonzero_samples"] > 0 for case in cases),
               "peak": max(case["peak"] for case in cases),
               "max_preroll_peak": max(case["preroll"]["peak"] for case in cases),
               "total_render_seconds": sum(case["render_seconds"] for case in cases),
               "native_comparison": "Not performed by reference exporter"}
    write_json(output / "summary.json", summary)
    write_json(output / "verification.json", {**summary, "manifest_sha256": sha((output / "manifest.json").read_bytes()),
                                               "merge_implementation_sha256": sha(Path(__file__).read_bytes()),
                                               "source_manifest_snapshots": origins})
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, action="append", required=True)
    parser.add_argument("--output", type=Path, default=ROOT / "analysis/recovered/preset-renders-full")
    args = parser.parse_args()
    print(json.dumps(merge(args.source, args.output), indent=2))


if __name__ == "__main__":
    main()
