"""Check externally constructed layouts, rejection boundaries, and recovered bank data."""

import copy
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("plasticz_state", ROOT / "scripts/plasticz_state.py")
codec = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(codec)


def fixture(order="little", names=True):
    """Independent two-program fixture includes -0, subnormal, and opaque name bytes."""
    prefix = "<" if order == "little" else ">"
    words = [0x80000000, 0x00000001, 0x3F800000, 0x3E800000] + [0] * 34
    words += [0x3F000000] * 38
    payload = struct.pack(prefix + "3I", 2, 38, 1) + struct.pack(prefix + "76I", *words)
    if names:
        payload += b"Name \0" + bytes(range(26))
        payload += bytes(range(128, 160))  # No terminator; codec must preserve all 32 bytes.
    return payload


class StateCodecTests(unittest.TestCase):
    def test_independent_layout_both_endians_with_and_without_names(self):
        for order in ("little", "big"):
            for names in (True, False):
                with self.subTest(order=order, names=names):
                    raw = fixture(order, names)
                    doc = codec.decode_chunk(raw)
                    self.assertEqual(doc["byte_order"], order)
                    self.assertEqual(doc["program_count"], 2)
                    self.assertEqual(doc["current_program"], 1)
                    self.assertEqual(doc["names_present"], names)
                    self.assertEqual(doc["programs"][0]["float32_bits_hex"][:4],
                                     ["80000000", "00000001", "3f800000", "3e800000"])
                    self.assertEqual(doc["programs"][1]["values"], [0.5] * 38)
                    if names:
                        self.assertEqual(doc["programs"][0]["name_display_latin1"], "Name ")
                        self.assertEqual(bytes.fromhex(doc["programs"][1]["name_storage_hex"]), bytes(range(128, 160)))
                    # Include serialization: negative zero and subnormal survive JSON too.
                    self.assertEqual(codec.encode_chunk(json.loads(json.dumps(doc))), raw)

    def test_endian_conversion_swaps_only_header_and_floats(self):
        doc = codec.decode_chunk(fixture("little"))
        self.assertEqual(codec.encode_chunk(doc, byte_order="big"), fixture("big"))
        self.assertEqual(codec.encode_chunk(codec.decode_chunk(fixture("big")), byte_order="little"), fixture("little"))

    def test_single_program_lower_boundary(self):
        for order, prefix in (("little", "<"), ("big", ">")):
            for names in (b"", bytes(range(32))):
                raw = struct.pack(prefix + "3I", 1, 38, 0) + fixture(order)[12:164] + names
                with self.subTest(order=order, names=bool(names)):
                    self.assertEqual(codec.encode_chunk(codec.decode_chunk(raw)), raw)

    def test_name_removal_is_explicit_and_cannot_invent_names(self):
        doc = codec.decode_chunk(fixture())
        self.assertEqual(codec.encode_chunk(doc, include_names=False), fixture(names=False))
        with self.assertRaisesRegex(codec.StateError, "Cannot include names"):
            codec.encode_chunk(codec.decode_chunk(fixture(names=False)), include_names=True)

    def test_exact_lengths_reject_partial_names_trailing_data_and_short_values(self):
        raw = fixture()
        for malformed in (b"", raw[:11], raw[:12], raw[:-1], raw + b"\0", fixture(names=False)[:-4], fixture(names=False) + b"\0"):
            with self.subTest(length=len(malformed)), self.assertRaises(codec.StateError):
                codec.decode_chunk(malformed)
        with self.assertRaises(codec.StateError):
            codec.decode_chunk(b"\0" * (codec.MAX_CHUNK_BYTES + 1))

    def test_reject_unsupported_header_counts_and_current_index(self):
        for order in ("<", ">"):
            for header in ((0, 38, 0), (129, 38, 0), (2, 37, 0), (2, 39, 0),
                           (2, 0xFFFFFFFF, 0), (2, 38, 2), (2, 38, 0xFFFFFFFF)):
                with self.subTest(order=order, header=header), self.assertRaises(codec.StateError):
                    codec.decode_chunk(struct.pack(order + "3I", *header) + bytes(304))

    def test_reject_nonfinite_or_out_of_range_float_words_in_both_endians(self):
        for order in ("little", "big"):
            prefix = "<" if order == "little" else ">"
            for word in (0x7FC00000, 0x7F800001, 0x7F800000, 0xFF800000, 0xBF000000, 0x3F800001):
                raw = bytearray(fixture(order))
                struct.pack_into(prefix + "I", raw, 12, word)
                with self.subTest(order=order, word=hex(word)), self.assertRaisesRegex(codec.StateError, "finite and within"):
                    codec.decode_chunk(raw)

    def test_reject_fxp_container_without_claiming_container_support(self):
        with self.assertRaisesRegex(codec.StateError, "FXP/FXB container"):
            codec.decode_chunk(b"CcnK" + bytes(192))

    def test_reject_conflicting_hex_and_decimal_data(self):
        doc = codec.decode_chunk(fixture())
        doc["programs"][0]["values"][3] = 0.5
        with self.assertRaisesRegex(codec.StateError, "disagree"):
            codec.encode_chunk(doc)
        # Users can deliberately edit decimal data after removing redundant bit words.
        del doc["programs"][0]["float32_bits_hex"]
        self.assertEqual(codec.decode_chunk(codec.encode_chunk(doc))["programs"][0]["values"][3], 0.5)

    def test_reject_malformed_json_structure(self):
        original = codec.decode_chunk(fixture())
        changes = [
            lambda d: d.update(program_count=True),
            lambda d: d.update(current_program=False),
            lambda d: d.update(byte_order="native"),
            lambda d: d.update(names_present="yes"),
            lambda d: d.update(names_present=False),
            lambda d: d["programs"].pop(),
            lambda d: d["programs"][0].update(index=1),
            lambda d: d["programs"][0].update(name_storage_hex="00"),
            lambda d: d["programs"][0].update(float32_bits_hex=["zzzzzzzz"] * 38),
            lambda d: d["programs"][0].update(values=[True] * 38),
            lambda d: d["programs"][0].update(values=[10 ** 1000] * 38),
        ]
        for change in changes:
            doc = copy.deepcopy(original)
            change(doc)
            with self.subTest(document=doc.keys()), self.assertRaises(codec.StateError):
                codec.encode_chunk(doc)

    def test_actual_recovered_factory_uses_values_then_names_not_interleaved(self):
        factory = json.loads((ROOT / "analysis/recovered/factory-presets.json").read_text())
        original = (ROOT / "analysis/source/PlastiCZ-i386").read_bytes()
        expected_values = b"".join(original[0x4B300 + i * 184 + 32:0x4B300 + (i + 1) * 184] for i in range(128))
        expected_names = b"".join(original[0x4B300 + i * 184:0x4B300 + i * 184 + 32] for i in range(128))
        doc = codec.from_factory(factory, current_program=127)
        raw = codec.encode_chunk(doc)
        self.assertEqual(raw, struct.pack("<3I", 128, 38, 127) + expected_values + expected_names)
        self.assertEqual(len(raw), 23564)
        self.assertEqual(len(codec.encode_chunk(doc, include_names=False)), 19468)
        self.assertEqual(doc["programs"][70]["name_display_latin1"], "ORG Funk ")

    def test_cli_encode_decode_and_reject_before_writing_output(self):
        script = str(ROOT / "scripts/plasticz_state.py")
        with tempfile.TemporaryDirectory(prefix="plasticz-state-test-") as temporary:
            root = Path(temporary)
            chunk, decoded, restored = root / "old.chunk", root / "decoded.json", root / "restored.chunk"
            chunk.write_bytes(fixture("big"))
            for arguments in (("--decode", chunk, decoded), ("--encode", decoded, restored)):
                result = subprocess.run([sys.executable, script, *map(str, arguments)], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(restored.read_bytes(), chunk.read_bytes())
            decoded.write_text('{"format":"first","format":"duplicate"}')
            rejected = root / "must-not-exist.chunk"
            result = subprocess.run([sys.executable, script, "--encode", str(decoded), str(rejected)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 2)
            self.assertIn("Duplicate JSON key", result.stderr)
            self.assertFalse(rejected.exists())


if __name__ == "__main__":
    unittest.main()
