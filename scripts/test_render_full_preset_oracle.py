#!/usr/bin/env python3
"""Exporter tests without substituting native DSP into original-reference renders."""
import copy
import math
import struct
import unittest
from unittest import mock

import render_full_preset_oracle as full


class FakeStereoOracle:
    """Deterministic scheduling test double; never used by the export CLI."""
    def initialize(self, parameters, rate):
        self.parameters = parameters.copy()
        self.countdown = 0.0
        self.elapsed = 0
        self.events = []
        self.held = False

    def read_float(self, offset):
        if offset != 0x406A44:
            raise AssertionError("Unexpected scheduler state")
        return self.countdown

    def write_float(self, offset, value):
        if offset != 0x406A44:
            raise AssertionError("Unexpected scheduler state")
        self.countdown = value

    def control_tick(self):
        self.events.append(("tick", self.elapsed))

    def note_on(self, note, velocity, channel):
        self.events.append(("on", self.elapsed, note, velocity, channel))
        self.held = True

    def note_off(self, note, channel):
        self.events.append(("off", self.elapsed, note, channel))
        self.held = False

    def render(self, frames):
        self.elapsed += frames
        left, right = ((0.125, -0.25) if self.held else (0.0001, 0.0002))
        return struct.pack("<f", left) * frames, struct.pack("<f", right) * frames


def config():
    return {"sample_rate": 10000, "preroll_frames": 512, "hold_frames": 97,
            "release_frames": 131, "block_size": 64, "note": 24,
            "velocity": full.f32(100 / 127), "channel": 0}


class FullExporterTests(unittest.TestCase):
    def test_interleave_preserves_words_and_channel_order(self):
        left = struct.pack("<3I", 0x80000000, 1, 0x3F800000)
        right = struct.pack("<3I", 0, 0x3F000000, 0xBF800000)
        expected = struct.pack("<6I", 0x80000000, 0, 1, 0x3F000000, 0x3F800000, 0xBF800000)
        self.assertEqual(full.interleave(left, right, 3), expected)
        with self.assertRaises(RuntimeError):
            full.interleave(left, right[:-1], 3)

    def test_stereo_float_wav_header_and_strict_decode(self):
        payload = struct.pack("<4f", 0.125, -0.25, 0.5, -0.75)
        wav = full.float_wav(payload, 44100)
        self.assertEqual(struct.unpack_from("<HHIIHH", wav, 20), (3, 2, 44100, 352800, 8, 32))
        self.assertEqual(struct.unpack_from("<I", wav, 44)[0], 2)
        self.assertEqual(full.wav_payload(wav, 44100), payload)
        for offset, replacement in ((22, struct.pack("<H", 1)), (44, struct.pack("<I", 3)),
                                    (4, struct.pack("<I", 0)), (32, struct.pack("<H", 4))):
            malformed = bytearray(wav)
            malformed[offset:offset + len(replacement)] = replacement
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                full.wav_payload(malformed)
        with self.assertRaises(ValueError):
            full.wav_payload(wav, 48000)
        with self.assertRaises(ValueError):
            full.float_wav(payload[:-1], 44100)

    def test_nonzero_preroll_and_exact_note_boundaries(self):
        oracle = FakeStereoOracle()
        parameters = [0.25] * 38
        parameters[37] = 1.0  # Exporter must not replace the factory oversampling value.
        payload, stats = full.render_case(oracle, parameters, config())
        self.assertEqual(oracle.parameters, parameters)
        self.assertEqual(len(payload), 228 * 8)
        self.assertEqual(stats["host_blocks"], 4)
        self.assertGreater(stats["preroll"]["peak"], 0)
        self.assertEqual(stats["preroll"]["frames"], 512)
        self.assertEqual([event[1] for event in oracle.events if event[0] == "on"], [512])
        self.assertEqual([event[1] for event in oracle.events if event[0] == "off"], [609])
        self.assertEqual(struct.unpack_from("<2f", payload), (0.125, -0.25))
        self.assertEqual(struct.unpack_from("<2f", payload, 96 * 8), (0.125, -0.25))
        self.assertAlmostEqual(struct.unpack_from("<2f", payload, 97 * 8)[0], 0.0001)
        self.assertEqual(stats["stereo_different_frames"], 228)
        again, repeated = full.render_case(oracle, parameters, config())
        self.assertEqual(again, payload)
        self.assertEqual(repeated["preroll"]["payload_sha256"], stats["preroll"]["payload_sha256"])

    def test_nonfinite_audio_is_rejected_without_normalization(self):
        for value in (math.inf, -math.inf, math.nan):
            with self.subTest(value=value), self.assertRaises(RuntimeError):
                full.audio_stats(struct.pack("<2f", value, 0.0))
        stats = full.audio_stats(struct.pack("<2f", 2.0, -3.0))
        self.assertEqual(stats["peak"], 3.0)  # Report over-range output; do not clip.

    def test_resume_verifies_parameters_hashes_rate_and_frame_count(self):
        cfg = config()
        program = {"index": 7, "values": [0.25] * 38}
        payload, stats = full.render_case(FakeStereoOracle(), program["values"], cfg)
        wav = full.float_wav(payload, cfg["sample_rate"])
        case = {"config_sha256": "test", "index": 7, "frames": 228,
                "wav_sha256": full.sha(wav), "payload_sha256": full.sha(payload),
                "parameters_sha256": full.sha(struct.pack("<38f", *program["values"]))}
        full.verify_resume(case, wav, cfg, "test", program)
        for key in ("config_sha256", "index", "frames", "wav_sha256", "payload_sha256", "parameters_sha256"):
            bad = copy.deepcopy(case)
            bad[key] = "bad"
            with self.subTest(key=key), self.assertRaises(ValueError):
                full.verify_resume(bad, wav, cfg, "test", program)

    def test_dependency_changes_fail_closed(self):
        full.verify_dependencies(full.LOADED_HASHES)
        with mock.patch.object(full, "sha", return_value="changed"):
            with self.assertRaisesRegex(RuntimeError, "dependency changed"):
                full.verify_dependencies(full.LOADED_HASHES)

    def test_unknown_import_guard_rejects_success_or_unrelated_failure(self):
        class UnknownImport:
            imports, extra_imports = {}, {}
            def invoke(self, address, arguments):
                raise RuntimeError("Unimplemented original import at 0x69000")
        self.assertTrue(full.verify_unknown_import_fails_closed(UnknownImport())["verified"])
        with mock.patch.object(UnknownImport, "invoke", return_value=0):
            with self.assertRaisesRegex(RuntimeError, "did not fail closed"):
                full.verify_unknown_import_fails_closed(UnknownImport())
        with mock.patch.object(UnknownImport, "invoke", side_effect=RuntimeError("different problem")):
            with self.assertRaisesRegex(RuntimeError, "different problem"):
                full.verify_unknown_import_fails_closed(UnknownImport())


if __name__ == "__main__":
    unittest.main()
