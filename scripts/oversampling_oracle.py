#!/usr/bin/env python3
"""Cached original-instruction 4x decimator, voice, and full-path comparisons.

Frozen FullDSPOracle dependencies are imported unchanged. Only the standalone
decimator cases temporarily redirect the original voice-loop entry to its
downsampler entry; all filter instructions and cleanup execute unchanged.
Unmodified original render code is used for all voice/full-path cases.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import struct

from full_dsp_oracle import FullDSPOracle
from render_preset_oracle import OriginalScheduler
from render_full_preset_oracle import StereoSchedulerAdapter
from voice_oracle import ROOT, OBJECT, OUT_POINTERS, OUT_LEFT, OUT_RIGHT, OVERSAMPLE
from x86_oracle import f32


class OversamplingOracle(FullDSPOracle):
    def decimator_state(self):
        return bytes(self.uc.mem_read(OBJECT + 0x40EB1C, 40))

    def dry(self, frames):
        if not 0 <= frames <= 32768:
            raise ValueError('Unsupported deterministic buffer range')
        self.uc.mem_write(OUT_LEFT, bytes(max(4, frames * 4)))
        self.uc.mem_write(OUT_RIGHT, bytes(max(4, frames * 4)))
        self.invoke(0xF8D0, struct.pack('<IIII', OBJECT, OUT_POINTERS, 0, frames), budget=200_000_000)
        return bytes(self.uc.mem_read(OUT_LEFT, frames * 4))

    def decimate(self, payload, frames):
        if len(payload) != frames * 16 or frames > 32768:
            raise ValueError('Wrong decimator input length')
        saved = bytes(self.uc.mem_read(0xFA3F, 5))
        # Keep original stack/PIC setup, skip the voice loop, enter the existing
        # inline downsampler using the original object's oversampling buffer.
        self.uc.mem_write(0xFA3F, b'\xe9' + struct.pack('<i', 0xFFF5 - 0xFA3F - 5))
        self.uc.ctl_remove_cache(0xF8D0, 0x10534)
        try:
            self.uc.mem_write(OVERSAMPLE, payload or bytes(4))
            return self.dry(frames)
        finally:
            self.uc.mem_write(0xFA3F, saved)
            self.uc.ctl_remove_cache(0xF8D0, 0x10534)


def sha(payload):
    return hashlib.sha256(payload).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'analysis/recovered/oversampling')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    presets = json.loads((ROOT / 'analysis/recovered/factory-presets.json').read_text())['programs']
    oracle = OversamplingOracle()
    commands = ['PLASTICZ_OVERSAMPLING_ORACLE 1']
    inputs, audio, states = bytearray(), bytearray(), bytearray()
    descriptions = []
    parameters = None
    scheduler = None

    def start(name, rate, full, values):
        nonlocal parameters, scheduler
        parameters = list(map(f32, values))
        oracle.initialize(parameters, float(rate))
        scheduler = OriginalScheduler(StereoSchedulerAdapter(oracle), rate)
        commands.append(f'case {name} {rate} {int(full)}')
        commands.append('parameters ' + ' '.join(repr(v) for v in parameters))
        commands.append('prepare')
        descriptions.append(dict(name=name, rate=rate, full=full, parameters=parameters.copy()))

    def change(index, value):
        parameters[index] = f32(value)
        oracle.invoke(0x12F4A, struct.pack('<IIf', OBJECT, index, parameters[index]), fpcw=0x037F)
        commands.append(f'parameter {index} {parameters[index]!r}')

    def control():
        oracle.control_tick()
        commands.append('control')

    def note(note_number, velocity=100 / 127):
        velocity = f32(velocity)
        oracle.note_on(note_number, velocity)
        commands.append(f'note_on {note_number} {velocity!r}')

    def off(note_number):
        oracle.note_off(note_number)
        commands.append(f'note_off {note_number}')

    def render(frames, host=False):
        payload = scheduler.render(frames) if host else oracle.dry(frames)
        commands.append(f'{"host" if host else "dry"} {frames} {len(audio)} {len(states)}')
        audio.extend(payload)
        states.extend(oracle.voices())
        states.extend(oracle.decimator_state())

    # Arbitrary fast-rate signals exercise the decimator independently of
    # oscillator shape, including cleanup, signed zeros, and tiny histories.
    for signal in ('impulses', 'noise', 'tiny'):
        p = presets[0]['values'].copy()
        p[37] = 1.0
        oracle.initialize(p)
        commands.append(f'downsampler {signal}')
        random = 0x504C435A
        position = 0
        for count in (0, 1, 3, 17, 44, 127, 1023, 1024, 1025, 4097, 512, 2048):
            values = []
            for i in range(count * 4):
                at = position + i
                random = (random * 1664525 + 1013904223) & 0xffffffff
                if signal == 'noise':
                    value = ((random >> 8) / 8388608.0 - 1.0) * (2.0 if at < 16384 else 0.0)
                elif signal == 'tiny':
                    value = (1.0 if at % 2 else -1.0) * (1e-14 if at < 4096 else 1e-38)
                else:
                    value = {0: 1.0, 29: -1.75, 211: 0.5, 12007: 2.0}.get(at, 0.0)
                values.append(value)
            payload = struct.pack('<' + 'f' * len(values), *values)
            expected = oracle.decimate(payload, count)
            commands.append(f'filter {count} {len(inputs)} {len(audio)} {len(states)}')
            inputs.extend(payload)
            audio.extend(expected)
            states.extend(oracle.decimator_state())
            position += count * 4
        commands.append('end')
        print('Downsampler', signal, 'complete', flush=True)

    # Also isolate original voice tests in a fresh emulator. This ensures no
    # translated block or patched instruction from the artificial entry path
    # can be reused by the unmodified-render comparisons below.
    oracle = OversamplingOracle()

    # Dry rendering isolates quarter-rate smoothing/phase and voice state from
    # the known non-44.1k full-synth constructor lifecycle distinction.
    for rate in (22050, 44100, 48000, 96000, 192000):
        p = presets[0]['values'].copy()
        p[35], p[37] = 1.0, 1.0
        start(f'dry-poly-{rate}', rate, False, p)
        render(0)
        render(1025)  # inactive voices leave downsampler state untouched
        control()
        for n in (24, 60, 96):
            note(n)
        for count in (0, 1, 43, 44, 45, 127, 1023, 1024, 1025, 4097):
            control()
            render(count)
        for mode in (0.0, 0.4999999701976776, 0.5, 1.0, 0.0, 1.0):
            change(37, mode)
            control()
            render(513)
        for n in (24, 60, 96):
            off(n)
        change(22, 0.0)
        for _ in range(48):
            control()
            render(127)
        commands.append('end')
        print('Dry voice/state', rate, 'complete', flush=True)

    # One actual factory preset per selector: host scheduling, effects and final
    # output remain enabled. This preserves real nonlinear/effect interactions.
    selected = {}
    for i, preset in enumerate(presets):
        effect = int(f32(preset['values'][28] * f32(10.99)))
        selected.setdefault(effect, i)
    for effect, index in sorted(selected.items()):
        p = presets[index]['values'].copy()
        p[37] = 1.0
        start(f'full-fx-{effect}-preset-{index}', 44100, True, p)
        render(512, True)
        note(24)
        for count in (0, 1, 43, 44, 45, 512, 1025, 127, 4097):
            render(count, True)
        note(67, 80 / 127)
        render(2048, True)
        change(37, 0.0)
        render(511, True)
        change(37, 0.5)
        render(1025, True)
        off(24)
        off(67)
        render(4097, True)
        commands.append('end')
        print('Full effect', effect, 'preset', index, 'complete', flush=True)

    payloads = {'commands.txt': ('\n'.join(commands) + '\n').encode(),
                'inputs.f32': bytes(inputs), 'audio.f32': bytes(audio), 'states.bin': bytes(states)}
    for name, payload in payloads.items():
        (args.output / name).write_bytes(payload)
    manifest = dict(schema=1, source_sha256=oracle.sha256,
                    isolation='Only standalone downsampler cases redirect 0xfa3f to 0xfff5 in emulator memory; original inline filter and end cleanup retained. Voice/full cases use unmodified original render instructions.',
                    sample_words=len(audio) // 4, state_bytes=len(states),
                    cases=descriptions, downsampler_cases=['impulses', 'noise', 'tiny'],
                    boundary='Frozen FullDSPOracle, current ARM libSystem math imports, original f32/x87 control contract. Dry rate tests avoid unrelated full-synth constructor-rate caveat.',
                    dependencies={name: sha((ROOT / 'scripts' / name).read_bytes()) for name in
                                  ('oversampling_oracle.py', 'full_dsp_oracle.py', 'voice_oracle.py', 'x86_oracle.py',
                                   'render_preset_oracle.py', 'render_full_preset_oracle.py')},
                    files={name: dict(bytes=len(payload), sha256=sha(payload)) for name, payload in payloads.items()})
    (args.output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print('Saved', len(audio) // 4, 'sample words and', len(states), 'state bytes', flush=True)


if __name__ == '__main__':
    main()
