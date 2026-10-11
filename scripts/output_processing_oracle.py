#!/usr/bin/env python3
"""Isolated original-instruction fixed-filter/limiter fixtures.

Imports, but never changes, the frozen FullDSPOracle. DSP routines are executed
from the fingerprinted i386 image; only the existing bounded runtime/math import
boundary is supplied by the host. No guessed DSP functions are substituted.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import struct

from full_dsp_oracle import FullDSPOracle, OBJECT, OUT_LEFT, OUT_RIGHT, OUT_POINTERS, ROOT
from x86_oracle import SOURCE_SHA256, f32

RATES = (22050, 44100, 48000, 88200, 96000, 192000)
PATTERN = (1, 7, 44, 0, 512, 3, 1024, 64, 17)
DYNAMICS = OBJECT + 0x40EB94


def sha(data):
    return hashlib.sha256(data).hexdigest()


def signals(count):
    left, right = [], []
    random_state = 0x504C435A
    for index in range(count):
        random_state = (1664525 * random_state + 1013904223) & 0xFFFFFFFF
        noise = f32(((random_state >> 8) / 8388608.0 - 1.0) * 3.5)
        if index < 128:
            l = 0.0
            r = -0.0
        elif index < 256:
            l = 4.0 if index == 128 else 0.0
            r = -7.0 if index == 173 else 0.0
        elif index < 768:
            l = 0.125
            r = -0.25
        elif index < 1536:
            l = 3.0
            r = -2.0
        elif index < 4608:
            l = noise
            r = f32(math.sin(index * 0.137) * (0.1 if index % 733 < 300 else 4.0))
        elif index < 6144:
            l = f32(math.sin(index * 0.071) * 0.15)
            r = f32(math.cos(index * 0.071) * 0.12)
        elif index < 6400:
            l = struct.unpack('<f', struct.pack('<I', 1 + (index % 97)))[0]
            r = -l
        else:
            l = r = 0.0
        left.append(l)
        right.append(r)
    return struct.pack('<' + 'f' * count, *left), struct.pack('<' + 'f' * count, *right)


def filter_address(oracle):
    return struct.unpack('<I', oracle.uc.mem_read(OBJECT + 0x40EB90, 4))[0]


def process(oracle, stage, left, right, count):
    oracle.uc.mem_write(OUT_LEFT, left)
    if stage == 'input':
        oracle.uc.mem_write(OUT_POINTERS, struct.pack('<II', OUT_LEFT, 0))
        address, target = 0x24C70, filter_address(oracle)
    else:
        oracle.uc.mem_write(OUT_RIGHT, right)
        oracle.uc.mem_write(OUT_POINTERS, struct.pack('<II', OUT_LEFT, OUT_RIGHT))
        address, target = 0x2D6AA, DYNAMICS
    oracle.invoke(address, struct.pack('<IIII', target, OUT_POINTERS, OUT_POINTERS, count))
    result = bytes(oracle.uc.mem_read(OUT_LEFT, 4 * count))
    if stage == 'output':
        result += bytes(oracle.uc.mem_read(OUT_RIGHT, 4 * count))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'analysis/recovered/output-processing')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    factory_path = ROOT / 'analysis/recovered/factory-presets.json'
    presets = json.loads(factory_path.read_text())['programs']
    oracle = FullDSPOracle()
    oracle.initialize(presets[0]['values'])
    filter_initial = bytes(oracle.uc.mem_read(filter_address(oracle), 0x160))
    output_initial = bytes(oracle.uc.mem_read(DYNAMICS, 0x209C))
    for preset in presets:
        oracle.initialize(preset['values'])
        if bytes(oracle.uc.mem_read(filter_address(oracle), 0x160)) != filter_initial:
            raise AssertionError('Synth preset modifies the fixed input-filter object')
        if bytes(oracle.uc.mem_read(DYNAMICS, 0x209C)) != output_initial:
            raise AssertionError('Synth preset modifies the fixed output-limiter object')
    print('All 128 factory parameter sets preserve both complete stage objects.', flush=True)

    commands = ['PLASTICZ_OUTPUT_ORACLE 1']
    inputs, expected = bytearray(), bytearray()
    cases = []
    for rate in RATES:
        oracle.initialize(presets[0]['values'], float(rate))
        if bytes(oracle.uc.mem_read(filter_address(oracle), 0x160)) != filter_initial:
            raise AssertionError('Original sample setter unexpectedly changed fixed input filter')
        params = struct.unpack('<I2f', oracle.uc.mem_read(DYNAMICS + 0x80, 4) +
                               oracle.uc.mem_read(DYNAMICS + 0x8C, 8))
        name = f'rate-{rate}'
        commands.append(f'case {name} {rate}')
        case = dict(name=name, sample_rate=rate, delay_samples=params[0],
                    attack_bits=struct.pack('<f', params[1]).hex(),
                    release_bits=struct.pack('<f', params[2]).hex(), segments=[])
        for reset, count in ((False, 12289), (True, 4097)):
            if reset:
                oracle.invoke(0x24656, struct.pack('<I', filter_address(oracle)), fpcw=0x037F)
                oracle.invoke(0x2D368, struct.pack('<I', DYNAMICS), fpcw=0x037F)
                commands.append('reset')
            left, right = signals(count)
            cursor, block = 0, 0
            while cursor < count:
                size = min(PATTERN[block % len(PATTERN)], count - cursor)
                l = left[cursor * 4:(cursor + size) * 4]
                r = right[cursor * 4:(cursor + size) * 4]
                for stage in ('input', 'output'):
                    result = process(oracle, stage, l, r, size)
                    channels = 1 if stage == 'input' else 2
                    commands.append(f'{stage} {size} {len(inputs)} {len(expected)}')
                    inputs += l + (r if channels == 2 else b'')
                    expected += result
                cursor += size
                block += 1
            case['segments'].append(dict(after_reset=reset, samples=count, calls_per_stage=block))
        commands.append('end')
        cases.append(case)
        print(name, 'delay', params[0], 'attack', params[1], 'release', params[2], flush=True)

    payloads = {'commands.txt': ('\n'.join(commands) + '\n').encode(),
                'inputs.f32': bytes(inputs), 'expected.f32': bytes(expected)}
    for name, data in payloads.items():
        (args.output / name).write_bytes(data)
    paths = ['scripts/x86_oracle.py', 'scripts/voice_oracle.py', 'scripts/full_dsp_oracle.py',
             'scripts/output_processing_oracle.py', 'analysis/recovered/factory-presets.json']
    manifest = dict(schema=1, source_binary_sha256=SOURCE_SHA256,
                    sources={p: sha((ROOT / p).read_bytes()) for p in paths},
                    boundary='Original i386 instructions; frozen FullDSPOracle current ARM libSystem imports; SSE nearest, gradual underflow; constructors/reset x87 0x037f, process 0x0c3f',
                    coverage='Fixed original mono filter and stereo limiter only; finite synthetic signals; 128 factory parameter invariance; six sample rates; reset and variable blocks including zero',
                    filter_initial_hex=filter_initial.hex(), output_initial_prefix_hex=output_initial[:0x9c].hex(),
                    block_pattern=list(PATTERN), cases=cases,
                    compared_float_words=len(expected) // 4,
                    files={name: dict(bytes=len(data), sha256=sha(data)) for name, data in payloads.items()})
    (args.output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print('Exported', len(expected) // 4, 'original sample words to', args.output, flush=True)


if __name__ == '__main__':
    main()
