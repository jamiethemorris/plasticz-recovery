"""Trust checks for the instruction oracle; run with project .venv Python.

On this machine Unicorn's native JIT requires execution outside the Codex sandbox.
"""
import ctypes
import struct
import unittest

from x86_oracle import COS_COUNT, SCRATCH, X86Oracle, f32


class OracleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.oracle = X86Oracle()

    def test_segment_file_offsets(self):
        imported = next(x for x in self.oracle.segments if x["name"] == "__IMPORT")
        self.assertEqual(imported["address"], "0x69000")
        self.assertEqual(imported["file_offset"], 0x59000)

    def test_x87_integer_conversion_truncates_both_signs(self):
        # fld dword[esp+4]; fistp dword[result]; fild dword[result]; ret.
        # This exercises the exact float->integer operation used by waveforms.
        target, result = SCRATCH + 0x200, SCRATCH + 0x300
        code = b"\xd9\x44\x24\x04\xdb\x1d" + struct.pack("<I", result)
        code += b"\xdb\x05" + struct.pack("<I", result) + b"\xc3"
        self.oracle.uc.mem_write(target, code)
        for value in (-1.75, -0.75, 0.75, 1.75, 8191.75):
            self.assertEqual(self.oracle.call(target, value), float(int(value)))

    def test_all_cosine_initializer_arguments_follow_float32_sse(self):
        # Independent argument construction checks the original initialization
        # loop, not a port of the waveform algorithms.
        cosf = ctypes.CDLL("/usr/lib/libSystem.B.dylib").cosf
        cosf.argtypes, cosf.restype = [ctypes.c_float], ctypes.c_float
        expected = b"".join(struct.pack("<f", cosf(f32(f32(i * f32(0.0003834952076431364))
                                                        - f32(3.1415927410125732))))
                            for i in range(COS_COUNT))
        self.assertEqual(self.oracle.cosine_bytes, expected)

    def test_scalar_offsets(self):
        for d in (0.0, 0.125, 0.5, 0.875, 1.0):
            expected = (0.0, 0.0, d, f32(0.5 * d), d, 0.5,
                        f32(f32(f32(-0.3) * d) + f32(0.7)), f32(f32(-0.38) * d))
            for index, value in enumerate(expected):
                self.assertEqual(self.oracle.offset(index, d), value, (index, d))

    def test_phase_endpoint_wraps(self):
        for wave in range(8):
            for d in (0.0, 0.25, 0.75, 1.0):
                self.assertEqual(self.oracle.waveform(wave, 0.0, d),
                                 self.oracle.waveform(wave, 1.0, d))


if __name__ == "__main__":
    unittest.main()
