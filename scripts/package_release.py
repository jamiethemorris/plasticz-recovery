#!/usr/bin/env python3
"""Package the verified AU/VST3 release, preserving outer bundle timestamps."""
from pathlib import Path
from datetime import datetime
import hashlib
import json
import plistlib
import re
import shutil
import subprocess
import tempfile
import zipfile
import argparse

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'build/native'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--public', action='store_true', help='Package refreshed docs and relative verification paths for publication')
    args = parser.parse_args()
    version = re.search(r'project\(PlastiCZRecovery VERSION (\d+\.\d+\.\d+)', (ROOT / 'CMakeLists.txt').read_text())[1]
    tag = version.replace('.', '')
    proof_path = ROOT / f'analysis/recovered/release-{version}/provenance.json'
    proof = json.loads(proof_path.read_text())
    assert proof['version'] == version
    destination = ROOT / ('build/distribution/public' if args.public else 'build/distribution')
    dest = destination / f'PlastiCZ Recovery {version} arm64'
    archive = dest.parent / (dest.name + '.zip')
    assert not dest.exists() and not archive.exists(), 'Refusing to overwrite an existing release'
    if args.public:
        # Published sources must still match the verified DSP, editor and artwork.
        for name, expected in proof['sources_sha256'].items():
            if name.startswith(('src/', 'assets/classic/')):
                assert sha(ROOT / name) == expected, f'Unverified source change: {name}'
    bundles = []
    for format, extension in [('AU', 'component'), ('VST3', 'vst3')]:
        bundle = BUILD / f'PlastiCZRecovery_artefacts/Release/{format}/PlastiCZ Recovery.{extension}'
        executable = bundle / 'Contents/MacOS/PlastiCZ Recovery'
        assert sha(executable) == proof['executables_sha256'][str(executable.relative_to(ROOT))], 'Unverified executable'
        assert plistlib.loads((bundle / 'Contents/Info.plist').read_bytes())['CFBundleShortVersionString'] == version
        assert bundle.stat().st_mtime >= executable.stat().st_mtime, 'Bundle date predates its executable'
        subprocess.run(['/usr/bin/codesign', '--verify', '--deep', '--strict', str(bundle)], check=True)
        bundles.append(bundle)
    for source in bundles:
        target = dest / source.name
        shutil.copytree(source, target)
        assert target.stat().st_mtime_ns == source.stat().st_mtime_ns
    files = {
        'README.md': ROOT / 'README.md',
        'INSTALL.txt': ROOT / 'packaging/INSTALL.txt',
        'TEST-RESULTS.txt': ROOT / 'analysis/emulator-comparison-results.txt',
        '4X-NOTES.txt': ROOT / 'analysis/oversampling-port-notes.txt',
        'INTERFACE-NOTES.txt': ROOT / 'analysis/native-ui-notes.txt',
        'verification-provenance.json': proof_path,
    }
    files.update({p.name: p for p in sorted((BUILD / f'ui-{tag}').glob('editor-*.png'))})
    for name, path in files.items():
        shutil.copy2(path, dest / name)
    if args.public:
        def relative_paths(value):
            if isinstance(value, dict):
                return {key: relative_paths(item) for key, item in value.items()}
            if isinstance(value, list):
                return [relative_paths(item) for item in value]
            if isinstance(value, str):
                return value.replace(str(ROOT) + '/', '')
            return value
        published_proof = relative_paths(proof)
        published_proof['public_package'] = {
            'source_commit': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
            'metadata_changes': 'Local fixture paths made project-relative; documentation refreshed; verified plug-in binaries unchanged.',
        }
        (dest / 'verification-provenance.json').write_text(json.dumps(published_proof, indent=2) + '\n')
        # Catch accidental publication of this machine's personal paths, including in binaries.
        for path in dest.rglob('*'):
            if path.is_file():
                assert str(Path.home()).encode() not in path.read_bytes(), f'Personal home path in {path.relative_to(dest)}'
    # Include directories explicitly: writing only files loses bundle dates.
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as output:
        for path in [dest, *sorted(dest.rglob('*'))]:
            output.write(path, path.relative_to(dest.parent))
    with zipfile.ZipFile(archive) as output:
        assert output.testzip() is None
        for source in bundles:
            item = output.getinfo(f'{dest.name}/{source.name}/')
            expected = list(datetime.fromtimestamp(source.stat().st_mtime).timetuple()[:6])
            expected[5] -= expected[5] % 2  # ZIP's DOS timestamp has two-second resolution.
            assert item.is_dir() and item.date_time == tuple(expected)
    # Check the same macOS extraction path users use, including signing and dates.
    with tempfile.TemporaryDirectory(prefix='plasticz-release-') as temporary:
        subprocess.run(['/usr/bin/ditto', '-xk', str(archive), temporary], check=True)
        for source in bundles:
            extracted = Path(temporary) / dest.name / source.name
            assert abs(extracted.stat().st_mtime - source.stat().st_mtime) < 2
            assert sha(extracted / 'Contents/MacOS/PlastiCZ Recovery') == sha(source / 'Contents/MacOS/PlastiCZ Recovery')
            subprocess.run(['/usr/bin/codesign', '--verify', '--deep', '--strict', str(extracted)], check=True)
    print(f'Packaged {archive}\nSHA-256 {sha(archive)}\nVerified AU/VST3 hashes, signatures and bundle dates after extraction.')


if __name__ == '__main__':
    main()
