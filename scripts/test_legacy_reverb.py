#!/usr/bin/env python3
"""Differential checks for native reverb against original i386 instructions.

Imports the frozen full oracle without modifying it. This tests the standalone
effect's constructor/prepare/configure/reset/process lifecycle, so the full
synth's documented non-44.1 kHz constructor limitation does not apply here.
"""
from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess

from full_dsp_oracle import FullDSPOracle, HEAP
from voice_oracle import OUT_LEFT, OUT_RIGHT, OUT_POINTERS, ROOT
from x86_oracle import SOURCE_SHA256


def sha(data):
    return hashlib.sha256(data).hexdigest()


def f32(x):
    return struct.unpack('<f', struct.pack('<f', x))[0]


def samples(kind, start, frames, channel):
    result = []
    for i in range(start, start + frames):
        if kind == 'silence':
            value = 0.0
        elif kind == 'impulse':
            value = (1.0 if channel == 0 else -0.375) if i == 0 else 0.0
        elif kind == 'noise':
            # Reproducible integer sequence, no additional host math boundary.
            word = (1664525 * ((i + 1) ^ (channel * 9176)) + 1013904223) & 0xffffffff
            word ^= word >> 13
            value = ((word & 0xffff) - 32768) / 65536
        else:
            raise ValueError(kind)
        result.append(f32(value))
    return result


class Native:
    def __init__(self, library):
        self.lib = ctypes.CDLL(str(library))
        self.lib.legacy_reverb_create.restype = ctypes.c_void_p
        self.lib.legacy_reverb_delete.argtypes = [ctypes.c_void_p]
        self.lib.legacy_reverb_prepare.argtypes = [ctypes.c_void_p, ctypes.c_float]
        self.lib.legacy_reverb_reset.argtypes = [ctypes.c_void_p]
        self.lib.legacy_reverb_reset_fresh.argtypes = [ctypes.c_void_p]
        self.lib.legacy_reverb_configure.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_float]
        self.lib.legacy_reverb_process.argtypes = [ctypes.c_void_p] + [ctypes.POINTER(ctypes.c_float)] * 4 + [ctypes.c_int]
        self.object = self.lib.legacy_reverb_create()

    def close(self):
        self.lib.legacy_reverb_delete(self.object)

    def prepare(self, rate):
        self.lib.legacy_reverb_prepare(self.object, rate)

    def reset(self):
        self.lib.legacy_reverb_reset(self.object)

    def reset_fresh(self):
        self.lib.legacy_reverb_reset_fresh(self.object)

    def configure(self, index, value):
        self.lib.legacy_reverb_configure(self.object, index, value)

    def render(self, left, right, in_place, seed):
        count = len(left)
        array = ctypes.c_float * count
        inputs = [array(*left), array(*right)]
        outputs = inputs if in_place else [array(*seed[0]), array(*seed[1])]
        self.lib.legacy_reverb_process(self.object, *inputs, *outputs, count)
        return b''.join(bytes(x) for x in outputs)


class Original:
    def __init__(self, oracle):
        self.oracle = oracle
        oracle.heap_cursor = HEAP
        oracle.allocations.clear()
        self.object = oracle.allocate(0x30c)
        self.call(0x25a8c)

    def call(self, address, suffix=b'', processing=False):
        kwargs = {} if processing else {'fpcw': 0x037f}
        self.oracle.invoke(address, struct.pack('<I', self.object) + suffix, **kwargs)

    def prepare(self, rate):
        self.call(0x26604, struct.pack('<f', rate))

    def reset(self):
        self.call(0x25c10)

    def configure(self, index, value):
        self.call(0x268d6, struct.pack('<If', index, value))

    def render(self, left, right, in_place, seed):
        count = len(left)
        inputs = [OUT_LEFT, OUT_RIGHT]
        outputs = inputs if in_place else [OUT_LEFT + 0x40000, OUT_RIGHT + 0x40000]
        for address, data in zip(inputs, [left, right]):
            self.oracle.uc.mem_write(address, struct.pack(f'<{count}f', *data))
        if not in_place:
            for address, data in zip(outputs, seed):
                self.oracle.uc.mem_write(address, struct.pack(f'<{count}f', *data))
        self.oracle.uc.mem_write(OUT_POINTERS, struct.pack('<4I', *inputs, *outputs))
        # Canary catches unexpected writes past the explicitly supplied spans.
        for address in inputs + outputs:
            self.oracle.uc.mem_write(address + 4 * count, b'\xa5' * 16)
        self.call(0x25ee0, struct.pack('<III', OUT_POINTERS, OUT_POINTERS + 8, count), processing=True)
        for address in inputs + outputs:
            assert bytes(self.oracle.uc.mem_read(address + 4 * count, 16)) == b'\xa5' * 16
        return b''.join(bytes(self.oracle.uc.mem_read(address, 4 * count)) for address in outputs)


def cases():
    def case(name, params=(), **kwargs):
        return dict(id=name, sample_rate=44100, parameters=list(params), frames=24000,
                    kind='impulse', in_place=True, block_sizes=[1, 7, 64, 511, 1024], **kwargs)
    result = [case('constructor-defaults'), case('unprepared-noop', prepare=False),
              case('configure-before-prepare', [(10, .5)], configure_first=True)]
    for index, (mix, room, damp) in enumerate([(0, 0, 0), (.25, .7, .5), (1, 1, 1), (.73, .13, .91)]):
        result.append(case(f'dispatcher-{index}', [(0, f32(f32(-.5 * mix) + 1)), (1, f32(.5 * mix)), (11, room), (12, damp)]))
    for index, params in enumerate([
            [(13, 0)], [(13, .25)], [(2, 0)], [(2, .37)], [(2, 1)],
            [(4, .375)], [(10, 1)], [(3, 1)],
            [(3, 1), (4, .05), (5, .21), (6, .1), (7, .9), (8, 0), (9, 1)],
            [(3, 1), (10, .333), (11, .999), (12, .74)], [(3, .9)], [(3, 2)]]):
        result.append(case(f'control-{index}', params))
    for index, rate in enumerate([8000, 22050, 48000, 96000, 192000]):
        item = case(f'rate-{rate}', [(10, .11), (11, .85), (12, .31)])
        item['sample_rate'] = rate
        result.append(item)
    for name, kwargs in [('silence-bias', {'kind': 'silence'}),
                         ('reset-preserves-history', {'kind': 'noise', 'reset_at': 12000}),
                         ('repeat-prepare-preserves-tail', {'prepare_at': 12000}),
                         ('rate-change-preserves-history', {'kind': 'noise', 'new_rate_at': 12000}),
                         ('out-of-place-additive', {'in_place': False, 'kind': 'noise'}),
                         ('out-of-place-independent', {'in_place': False, 'kind': 'noise', 'parameters': [(3, 1), (13, .25)]}),
                         ('mid-tail-configure', {'configure_at': 12000}),
                         ('single-blocks', {'block_sizes': [4096]}),
                         ('release-long', {'frames': 120000})]:
        item = case(name)
        item.update(kwargs)
        result.append(item)
    for index, params in enumerate([[], [(10, .123), (11, .98), (12, .21)],
                                   [(3, 1), (4, .075), (5, .71), (2, .91)]]):
        item = case(f'fresh-reset-{index}', params)
        item.update(kind='noise', fresh_at=12000)
        result.append(item)
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, default=ROOT / 'analysis/recovered/legacy-reverb')
    parser.add_argument('--library', type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    library = args.library or args.output / 'libcheck_legacy_reverb.dylib'
    command = ['clang++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-ffp-contract=off', '-fno-fast-math',
               '-dynamiclib', '-Isrc', 'tests/check_legacy_reverb.cpp', 'src/LegacyReverb.cpp', '-o', str(library)]
    if not args.library:
        subprocess.run(command, cwd=ROOT, check=True)
    dependencies = ['scripts/full_dsp_oracle.py', 'scripts/voice_oracle.py', 'scripts/x86_oracle.py',
                    'scripts/test_legacy_reverb.py', 'src/LegacyReverb.h', 'src/LegacyReverb.cpp', 'tests/check_legacy_reverb.cpp']
    hashes = {p: sha((ROOT / p).read_bytes()) for p in dependencies}
    oracle = FullDSPOracle()
    report = dict(protocol='plasticz-standalone-reverb-v1', source_sha256=SOURCE_SHA256,
                  dependencies=hashes, compiler_command=command, native_library_sha256=sha(library.read_bytes()),
                  boundary='Original i386 constructor/prepare/configure/reset/process under Unicorn; imported cosf/sinf supplied by current libSystem.',
                  cases=[])
    original_audio = bytearray()
    for case in cases():
        original = Original(oracle)
        native = Native(library)
        pair = [original, native]
        try:
            if case.get('configure_first'):
                for obj in pair:
                    for index, value in case['parameters']:
                        obj.configure(index, value)
            if case.get('prepare', True):
                for obj in pair:
                    obj.prepare(case['sample_rate'])
            if not case.get('configure_first'):
                for obj in pair:
                    for index, value in case['parameters']:
                        obj.configure(index, value)
            rendered = [bytearray(), bytearray()]
            start = step = 0
            while start < case['frames']:
                if start == case.get('fresh_at'):
                    original = Original(oracle)
                    original.prepare(case['sample_rate'])
                    for index, value in case['parameters']:
                        original.configure(index, value)
                    native.reset_fresh()
                    pair = [original, native]
                for key in ('reset_at', 'prepare_at', 'new_rate_at', 'configure_at'):
                    if start == case.get(key):
                        for obj in pair:
                            if key == 'reset_at': obj.reset()
                            elif key == 'prepare_at': obj.prepare(case['sample_rate'])
                            elif key == 'new_rate_at': obj.prepare(48000)
                            else:
                                obj.configure(10, .037)
                                obj.configure(11, .31)
                                obj.configure(12, .92)
                count = min(case['block_sizes'][step % len(case['block_sizes'])], case['frames'] - start)
                for key in ('reset_at', 'prepare_at', 'new_rate_at', 'configure_at', 'fresh_at'):
                    if start < case.get(key, -1) < start + count:
                        count = case[key] - start
                left, right = [samples(case['kind'], start, count, channel) for channel in range(2)]
                seed = [[f32(.03125 * (channel + 1))] * count for channel in range(2)]
                for index, obj in enumerate(pair):
                    rendered[index].extend(obj.render(left, right, case['in_place'], seed))
                start += count
                step += 1
            reference, actual = map(bytes, rendered)
            assert all(math.isfinite(x[0]) for x in struct.iter_unpack('<f', reference))
            differing = sum(a != b for a, b in zip(struct.iter_unpack('<I', reference), struct.iter_unpack('<I', actual)))
            item = dict(case, exact=reference == actual, differing_channel_samples=differing,
                        original_sha256=sha(reference), native_sha256=sha(actual),
                        original_byte_offset=len(original_audio), byte_count=len(reference))
            if differing:
                first = next(i for i, (a, b) in enumerate(zip(struct.iter_unpack('<I', reference), struct.iter_unpack('<I', actual))) if a != b)
                item['first_different_storage_sample'] = first
                item['first_values'] = [struct.unpack_from('<f', x, first * 4)[0] for x in [reference, actual]]
            original_audio.extend(reference)
            report['cases'].append(item)
            print(case['id'], 'PASS' if item['exact'] else 'FAIL', differing, item.get('first_values', ''), flush=True)
        finally:
            native.close()
    assert hashes == {p: sha((ROOT / p).read_bytes()) for p in dependencies}, 'Pinned source changed during fixture run'
    report['all_exact'] = all(c['exact'] for c in report['cases'])
    report['total_channel_samples'] = len(original_audio) // 4
    report['original_audio_storage'] = 'original-block-planar.f32'
    report['original_audio_sha256'] = sha(original_audio)
    report['extended_imports'] = dict(oracle.extended_calls)
    (args.output / report['original_audio_storage']).write_bytes(original_audio)
    (args.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    return 0 if report['all_exact'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
