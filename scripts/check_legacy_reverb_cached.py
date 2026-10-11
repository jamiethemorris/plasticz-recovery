#!/usr/bin/env python3
"""Replay saved original reverb fixtures with native code only (no Unicorn)."""
import argparse
import ctypes
import hashlib
import json
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[1]


def f32(value):
    return struct.unpack('<f', struct.pack('<f', value))[0]


def input_samples(kind, start, count, channel):
    values = []
    for index in range(start, start + count):
        if kind == 'silence':
            value = 0.0
        elif kind == 'impulse':
            value = (1.0 if channel == 0 else -.375) if index == 0 else 0.0
        elif kind == 'noise':
            word = (1664525 * ((index + 1) ^ (channel * 9176)) + 1013904223) & 0xffffffff
            word ^= word >> 13
            value = ((word & 0xffff) - 32768) / 65536
        else:
            raise ValueError('Unknown fixture input: ' + kind)
        values.append(f32(value))
    return values


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--library', required=True, type=Path)
    parser.add_argument('--reference', type=Path, default=ROOT / 'analysis/recovered/legacy-reverb')
    args = parser.parse_args()
    report = json.loads((args.reference / 'report.json').read_text())
    if report['protocol'] != 'plasticz-standalone-reverb-v1' or not report['all_exact']:
        raise ValueError('Expected complete v1 original reverb evidence')
    reference = (args.reference / report['original_audio_storage']).read_bytes()
    if hashlib.sha256(reference).hexdigest() != report['original_audio_sha256']:
        raise ValueError('Original fixture payload hash mismatch')
    if len(reference) != report['total_channel_samples'] * 4:
        raise ValueError('Original fixture payload size mismatch')
    lib = ctypes.CDLL(str(args.library.resolve()))
    lib.legacy_reverb_create.restype = ctypes.c_void_p
    for name in ['delete', 'reset', 'reset_fresh']:
        getattr(lib, 'legacy_reverb_' + name).argtypes = [ctypes.c_void_p]
    lib.legacy_reverb_prepare.argtypes = [ctypes.c_void_p, ctypes.c_float]
    lib.legacy_reverb_configure.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_float]
    lib.legacy_reverb_process.argtypes = [ctypes.c_void_p] + [ctypes.POINTER(ctypes.c_float)] * 4 + [ctypes.c_int]
    failures = []
    seen = set()
    storage_offset = 0
    for case in report['cases']:
        if case['id'] in seen or case['original_byte_offset'] != storage_offset:
            raise ValueError('Duplicate case or inconsistent fixture offsets')
        seen.add(case['id'])
        size = case['frames'] * 8
        expected = reference[storage_offset:storage_offset + size]
        if case['byte_count'] != size or hashlib.sha256(expected).hexdigest() != case['original_sha256']:
            raise ValueError('Invalid per-case fixture size or hash')
        storage_offset += size
        object_ = lib.legacy_reverb_create()
        try:
            def configure():
                for index, value in case['parameters']:
                    lib.legacy_reverb_configure(object_, index, value)
            if case.get('configure_first'): configure()
            if case.get('prepare', True): lib.legacy_reverb_prepare(object_, case['sample_rate'])
            if not case.get('configure_first'): configure()
            actual = bytearray()
            start = step = 0
            event_keys = ['reset_at', 'prepare_at', 'new_rate_at', 'configure_at', 'fresh_at']
            while start < case['frames']:
                if start == case.get('reset_at'): lib.legacy_reverb_reset(object_)
                if start == case.get('prepare_at'): lib.legacy_reverb_prepare(object_, case['sample_rate'])
                if start == case.get('new_rate_at'): lib.legacy_reverb_prepare(object_, 48000)
                if start == case.get('fresh_at'): lib.legacy_reverb_reset_fresh(object_)
                if start == case.get('configure_at'):
                    for index, value in [(10, .037), (11, .31), (12, .92)]:
                        lib.legacy_reverb_configure(object_, index, value)
                count = min(case['block_sizes'][step % len(case['block_sizes'])], case['frames'] - start)
                for key in event_keys:
                    if start < case.get(key, -1) < start + count:
                        count = case[key] - start
                array = ctypes.c_float * count
                inputs = [array(*input_samples(case['kind'], start, count, channel)) for channel in range(2)]
                outputs = inputs if case['in_place'] else [array(*([f32(.03125 * (channel + 1))] * count)) for channel in range(2)]
                lib.legacy_reverb_process(object_, *inputs, *outputs, count)
                actual.extend(b''.join(bytes(output) for output in outputs))
                start += count
                step += 1
            if actual != expected:
                differing = sum(x != y for x, y in zip(struct.iter_unpack('<I', actual), struct.iter_unpack('<I', expected)))
                failures.append((case['id'], differing))
                print(f"FAIL {case['id']}: {differing} float32 words differ")
        finally:
            lib.legacy_reverb_delete(object_)
    if storage_offset != len(reference):
        raise ValueError('Unused original fixture bytes')
    print(f"Native reverb cached fixtures: {len(seen) - len(failures)}/{len(seen)} exact, {len(reference) // 4:,} channel samples")
    return 1 if failures else 0


if __name__ == '__main__':
    raise SystemExit(main())
