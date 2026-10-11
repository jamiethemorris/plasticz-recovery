"""Trust checks for the isolated original voice/control/dry-render oracle."""
import json
import math
from pathlib import Path
import struct
import unittest

from voice_oracle import VoiceOracle, OBJECT, OUT_LEFT, ROOT, VOICE_STRIDE, VOICE_COUNT, f32


class VoiceOracleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.oracle = VoiceOracle()
        cls.factory = json.loads((ROOT / "analysis/recovered/factory-presets.json").read_text())["programs"]

    def initialize(self, preset=0, rate=44100):
        parameters = self.factory[preset]["values"].copy()
        parameters[37] = 0
        self.oracle.initialize(parameters, rate)

    def test_constructor_scalars_and_silence(self):
        self.initialize(rate=48000)
        self.assertEqual(self.oracle.read_float(0x406344), 48000)
        self.assertEqual(self.oracle.read_float(0x406348), f32(f32(1 / 48000) * 44100))
        self.assertEqual(self.oracle.read_float(0x40632C), 0.5)
        self.assertEqual(self.oracle.read_float(0x40633C), 1.0)
        self.assertEqual(self.oracle.voices(), bytes(VOICE_STRIDE * VOICE_COUNT))
        self.assertEqual(self.oracle.render(256), bytes(256 * 4))

    def test_polyphonic_note_on_and_release_fields(self):
        self.initialize(preset=8)
        self.oracle.control_tick()
        self.oracle.note_on(60, 0.75)
        self.oracle.note_on(67, 0.5)
        before = self.oracle.voices()
        self.assertEqual((before[0], before[VOICE_STRIDE]), (60, 67))
        self.assertEqual((before[0x25], before[VOICE_STRIDE + 0x25]), (1, 1))
        self.assertEqual(struct.unpack_from("<f", before, 4)[0], 0.75)
        self.oracle.control_tick()
        self.oracle.note_off(60)
        after = self.oracle.voices()
        self.assertEqual((after[0x25], after[VOICE_STRIDE + 0x25]), (0, 1))
        self.assertEqual((after[0x14], after[0x24]), (1, 1))
        self.assertEqual(after[0x10:0x14], after[0x08:0x0C])
        self.assertEqual(after[0x20:0x24], after[0x18:0x1C])

    def test_nonzero_render_offset_preserves_sequence_and_guards(self):
        def run(offset):
            self.initialize()
            self.oracle.control_tick()
            self.oracle.note_on(60, 0.8)
            self.oracle.control_tick()
            end = OUT_LEFT + (offset + 64) * 4
            self.oracle.uc.mem_write(end, b"\xa5" * 16)
            audio = self.oracle.render(64, offset)
            self.assertEqual(bytes(self.oracle.uc.mem_read(end, 16)), b"\xa5" * 16)
            self.assertEqual(bytes(self.oracle.uc.mem_read(OUT_LEFT, offset * 4)), bytes(offset * 4))
            self.assertTrue(any(value != 0 for value in struct.unpack("<64f", audio)))
            return audio, self.oracle.voices()
        self.assertEqual(run(0), run(17))

    def test_unhandled_import_fails_closed(self):
        with self.assertRaisesRegex(RuntimeError, "Unimplemented original import"):
            self.oracle.invoke(0x69000, b"")

    def test_replay_published_audio_and_all_voice_snapshots(self):
        directory = ROOT / "analysis/recovered/voice-oracle"
        manifest = json.loads((directory / "manifest.json").read_text())
        voices = (directory / manifest["voice_storage"]).read_bytes()
        audio = (directory / manifest["audio_storage"]).read_bytes()
        for case in manifest["cases"]:
            for index, step in enumerate(case["steps"]):
                with self.subTest(case=case["id"], step=index):
                    action = step["action"]
                    if action == "initialize":
                        self.oracle.initialize(case["parameters"], case["sample_rate"])
                    elif action == "control":
                        self.oracle.control_tick()
                    elif action == "note_on":
                        self.oracle.note_on(step["note"], step["velocity"], step["channel"])
                    elif action == "note_off":
                        self.oracle.note_off(step["note"], step["channel"])
                    elif action == "render":
                        actual = self.oracle.render(step["frames"], step["offset"])
                        begin = step["audio_byte_offset"]
                        self.assertEqual(actual, audio[begin:begin + step["frames"] * 4])
                    else:
                        self.fail(f"Unknown fixture action: {action}")
                    begin = step["voices_byte_offset"]
                    self.assertEqual(self.oracle.voices(), voices[begin:begin + VOICE_COUNT * VOICE_STRIDE])


if __name__ == "__main__":
    unittest.main()
