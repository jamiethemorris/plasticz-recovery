#!/usr/bin/env python3
"""Verify and preserve the current release's test results and local build inputs."""
from pathlib import Path
from datetime import datetime, timezone
import csv
import hashlib
import json
import plistlib
import shutil
import struct
import subprocess
import argparse
import re

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'build/native'
VERSION = re.search(r'project\(PlastiCZRecovery VERSION (\d+\.\d+\.\d+)', (ROOT / 'CMakeLists.txt').read_text())[1]
TAG = VERSION.replace('.', '')
DEST = ROOT / f'analysis/recovered/release-{VERSION}'

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def payload(path):
    data = path.read_bytes()
    assert data[:4] == b'RIFF' and data[8:12] == b'WAVE'
    at = 12
    while at + 8 <= len(data):
        count = struct.unpack_from('<I', data, at + 4)[0]
        assert at + 8 + count <= len(data)
        if data[at:at + 4] == b'data':
            return data[at + 8:at + 8 + count]
        at += 8 + count + (count & 1)
    raise ValueError('Missing WAV payload')

def comparisons(name, count, group_size):
    rows = list(csv.DictReader((BUILD / name).open()))
    assert len(rows) == count
    groups = {}
    for row in rows:
        assert int(row['bit_mismatches']) == 0
        for field in ('max_abs_error', 'rms_error'):
            assert float(row.get(field, '0')) == 0
        key = row['format'] + ', FZ=' + row['host_flush_subnormals']
        groups.setdefault(key, []).append(row)
    assert len(groups) == 4 and all(len(g) == group_size for g in groups.values())
    return {key: {'cases': len(group), 'channel_samples': sum(int(r['channel_samples']) for r in group),
                  'bit_mismatches': 0} for key, group in groups.items()}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--safety-baseline', help='Reuse a verified release safety report only if its DSP inputs are unchanged')
    args = parser.parse_args()
    assert not DEST.exists(), f'Release evidence already exists: {DEST}'
    full = comparisons(f'plugin-full-{TAG}.csv', 512, 128)
    oversampling = comparisons(f'plugin-4x-{TAG}.csv', 44, 11)
    assert '100% tests passed, 0 tests failed out of 12' in (BUILD / f'ctest-{TAG}.log').read_text()
    assert 'Editor verification passed:' in (BUILD / f'editor-{TAG}.log').read_text()
    safety_source = args.safety_baseline or VERSION
    safety_log = BUILD / f'safety-{safety_source.replace(".", "")}-sanitized.log'
    if args.safety_baseline:
        baseline = json.loads((ROOT / f'analysis/recovered/release-{safety_source}/provenance.json').read_text())
        inputs = [*sorted((ROOT / 'src').glob('*.h')), *sorted((ROOT / 'src').glob('*.cpp')),
                  ROOT / 'tests/check_effects_safety.cpp']
        for path in inputs:
            if path.name in ('PluginEditor.cpp', 'PluginEditor.h'):
                continue
            assert sha(path) == baseline['sources_sha256'][str(path.relative_to(ROOT))], f'Safety input changed: {path}'
        assert sha(safety_log) == baseline['reports_sha256'][str(safety_log.relative_to(ROOT))]
    assert 'zero C++ heap allocations/deallocations' in safety_log.read_text()
    assert 'ERROR:' not in safety_log.read_text()
    assert 'runtime error:' not in safety_log.read_text()
    original = ROOT / 'analysis/recovered/preset-renders-full'
    native = BUILD / 'preset-regression-full/native'
    native_manifest = json.loads((native / 'native-manifest.json').read_text())
    assert native_manifest['bit_mismatches'] == 0 and native_manifest['preset_count'] == 128
    audio_hashes = {}
    for index in range(128):
        name = f'preset-{index:03d}.wav'
        audio = payload(native / name)
        assert len(audio) == 264600 * 8 and audio == payload(original / 'reference' / name)
        assert not any(payload(BUILD / 'preset-regression-full/difference' / name))
        audio_hashes[name] = hashlib.sha256(audio).hexdigest()
    fixture = ROOT / 'analysis/recovered/oversampling'
    fixture_manifest = json.loads((fixture / 'manifest.json').read_text())
    for name, item in fixture_manifest['files'].items():
        assert sha(fixture / name) == item['sha256']
    executables = [BUILD / 'render_presets', BUILD / 'check_oversampling', BUILD / 'check_plugin_editor',
                   BUILD / 'check_plugin_host_artefacts/Release/check_plugin_host']
    for format, extension in [('AU', 'component'), ('VST3', 'vst3')]:
        bundle = BUILD / f'PlastiCZRecovery_artefacts/Release/{format}/PlastiCZ Recovery.{extension}'
        with (bundle / 'Contents/Info.plist').open('rb') as stream:
            assert plistlib.load(stream)['CFBundleShortVersionString'] == VERSION
        subprocess.run(['/usr/bin/codesign', '--verify', '--deep', '--strict', str(bundle)], check=True)
        executable = bundle / 'Contents/MacOS/PlastiCZ Recovery'
        assert 'arm64' in subprocess.check_output(['/usr/bin/file', str(executable)], text=True)
        assert bundle.stat().st_mtime >= executable.stat().st_mtime, 'Stale bundle modification time'
        for log in (f'plugin-full-{TAG}.log', f'plugin-4x-{TAG}.log'):
            assert (BUILD / log).stat().st_mtime >= executable.stat().st_mtime
        executables.append(executable)
    sources = [ROOT / 'CMakeLists.txt', *sorted((ROOT / 'src').glob('*.h')), *sorted((ROOT / 'src').glob('*.cpp')),
               *sorted((ROOT / 'tests').glob('*.cpp')), *sorted((ROOT / 'assets/classic').glob('*')),
               ROOT / 'scripts/oversampling_oracle.py', ROOT / 'scripts/extract_ui_layout.py',
               ROOT / 'scripts/package_release.py', Path(__file__)]
    reports = [BUILD / name for name in (f'ctest-{TAG}.log', f'plugin-full-{TAG}.csv', f'plugin-full-{TAG}.log',
                f'plugin-4x-{TAG}.csv', f'plugin-4x-{TAG}.log', f'editor-{TAG}.log')]
    reports.append(safety_log)
    reports += [fixture / 'manifest.json', fixture / 'native-verification.json',
                ROOT / 'analysis/oversampling-port-notes.txt', ROOT / 'analysis/original-ui-notes.txt',
                ROOT / 'analysis/native-ui-notes.txt', ROOT / 'analysis/emulator-comparison-results.txt',
                *sorted((BUILD / f'ui-{TAG}').glob('editor-*.png'))]
    for path in sources + reports:
        target = DEST / 'snapshot' / path.relative_to(ROOT)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, target)
    result = {'version': VERSION, 'recorded_utc': datetime.now(timezone.utc).isoformat(),
              'safety_report_release': safety_source,
              'safety_report_reused_for_unchanged_sources': bool(args.safety_baseline),
              'architecture': 'arm64', 'native_full_result': native_manifest,
              'actual_bundle_full_results': full, 'actual_bundle_4x_results': oversampling,
              'sources_sha256': {str(p.relative_to(ROOT)): sha(p) for p in sources},
              'reports_sha256': {str(p.relative_to(ROOT)): sha(p) for p in reports},
              'executables_sha256': {str(p.relative_to(ROOT)): sha(p) for p in executables},
              'native_full_audio_sha256': audio_hashes, 'bundle_signatures': 'Strict complete-bundle checks passed',
              'limits': ['Full factory bank protocol is C1/velocity100/44100Hz/120BPM, original factory4x off.',
                         '4x full-path references cover11FX scenarios at44100Hz, dry/state tests five sample rates.',
                         'Oracle executes original i386 instructions with current ARM libSystem math imports.',
                         'Classic uses original assets and recovered positions; Carbon runtime UI was not available.',
                         'Host tests load bundles directly; no DAW installation or automatic old-session replacement.']}
    (DEST / 'provenance.json').write_text(json.dumps(result, indent=2) + '\n')
    print('Release verified: 128 full renders, 512 actual-bundle factory comparisons, 44 actual-bundle 4x comparisons, editor, 12 CTests, sanitizers, signatures and snapshots.')

if __name__ == '__main__':
    main()
