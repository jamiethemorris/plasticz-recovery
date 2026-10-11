#!/usr/bin/env python3
"""Record local build/test provenance after the strict full audio suites pass."""
from pathlib import Path
from datetime import datetime,timezone
import csv,hashlib,json,struct,subprocess
ROOT=Path(__file__).resolve().parents[1]
SUITE=ROOT/'analysis/recovered/preset-renders-full'
def sha(data):return hashlib.sha256(data).hexdigest()
def filehash(p):return sha(p.read_bytes())
def payload(p):
 data=p.read_bytes();assert data[:4]==b'RIFF' and data[8:12]==b'WAVE'
 at=12
 while at<len(data):
  n=struct.unpack_from('<I',data,at+4)[0]
  if data[at:at+4]==b'data':return data[at+8:at+8+n]
  at+=8+n+(n&1)
 raise ValueError('No audio payload')
def main():
 original=json.loads((SUITE/'manifest.json').read_text())
 native=json.loads((SUITE/'native/native-manifest.json').read_text())
 assert original['full_protocol_complete'] and native['bit_mismatches']==0
 cases=[]
 for case in original['cases']:
  i=case['index'];name=f'preset-{i:03d}.wav';ref=SUITE/'reference'/name;actual=SUITE/'native'/name
  diff=SUITE/'difference'/name
  assert filehash(ref)==case['wav_sha256']
  audio=payload(actual);assert len(audio)==264600*8 and audio==payload(ref)
  assert not any(payload(diff))
  cases.append({'preset':i,'native_wav_sha256':filehash(actual),'audio_sha256':sha(audio),'difference_wav_sha256':filehash(diff)})
 assert [c['preset'] for c in cases]==list(range(128))
 rows=list(csv.DictReader((SUITE/'plugin-comparison.csv').open()));assert len(rows)==512
 groups={}
 for row in rows:
  assert int(row['bit_mismatches'])==0 and float(row['max_abs_error'])==0 and float(row['rms_error'])==0
  assert int(row['reference_channels'])==2 and int(row['channel_samples'])==529200
  key=row['format']+', host FZ='+row['host_flush_subnormals'];groups.setdefault(key,[]).append(int(row['preset']))
 assert len(groups)==4 and all(sorted(v)==list(range(128)) for v in groups.values())
 executables=['build/dsp-effects/render_presets','build/native/render_presets','build/native/check_plugin_host_artefacts/Release/check_plugin_host']
 bundles=[]
 for format,suffix in [('AU','component'),('VST3','vst3')]:
  bundle=ROOT/f'build/native/PlastiCZRecovery_artefacts/Release/{format}/PlastiCZ Recovery.{suffix}'
  subprocess.run(['/usr/bin/codesign','--verify','--deep','--strict',str(bundle)],check=True)
  executable=bundle/'Contents/MacOS/PlastiCZ Recovery';executables.append(str(executable.relative_to(ROOT)));bundles.append(str(bundle.relative_to(ROOT)))
 sources=[ROOT/'CMakeLists.txt',*sorted((ROOT/'src').glob('*.h')),*sorted((ROOT/'src').glob('*.cpp')),
          *sorted((ROOT/'tests').glob('*.cpp')),ROOT/'scripts/effects_oracle.py',ROOT/'scripts/effects_sequence_oracle.py']
 source_hashes={str(p.relative_to(ROOT)):filehash(p) for p in sources}
 snapshot=SUITE/'native/source-snapshot'
 for p in sources:
  dst=snapshot/p.relative_to(ROOT);dst.parent.mkdir(parents=True,exist_ok=True);dst.write_bytes(p.read_bytes())
 reports=['analysis/recovered/preset-renders-full/native/native-manifest.json','analysis/recovered/preset-renders-full/native/comparison.csv',
          'analysis/recovered/preset-renders-full/plugin-comparison.csv','analysis/recovered/preset-renders-full/plugin-comparison.log',
          'analysis/recovered/effects/manifest.json','analysis/recovered/effects/native-comparison.log',
          'analysis/recovered/effects-sequence/manifest.json','analysis/recovered/effects-sequence/native-comparison.log',
          'build/native/full-final-ctest.log','build/native/final-sequence-ctest.log']
 result={'recorded_utc':datetime.now(timezone.utc).isoformat(),'version':'0.2.0','architecture':'arm64',
  'reference_config_sha256':original['config_sha256'],'reference_manifest_sha256':filehash(SUITE/'manifest.json'),
  'native_renderer_command':'build/dsp-effects/render_presets --full --reference analysis/recovered/preset-renders-full/reference --output analysis/recovered/preset-renders-full/native --difference analysis/recovered/preset-renders-full/difference',
  'plugin_host_argv':['build/native/check_plugin_host_artefacts/Release/check_plugin_host',bundles[1],bundles[0],'--full','analysis/recovered/preset-renders-full/reference','analysis/recovered/preset-renders-full/plugin-comparison.csv'],
  'compiler_flags':['-ffp-contract=off','-fno-fast-math'],'sources_sha256':source_hashes,
  'executables_sha256':{p:filehash(ROOT/p) for p in executables},'bundle_signatures':'codesign --verify --deep --strict passed for both complete bundles',
  'native_result':native,'plugin_results':{k:{'presets':len(v),'channel_samples':67737600,'bit_mismatches':0} for k,v in groups.items()},
  'reports_sha256':{p:filehash(ROOT/p) for p in reports},'native_files':cases,
  'ctest':'Ten cached checks passed in full-final-ctest.log; newly added effects_sequence passed separately in final-sequence-ctest.log. No DSP changes between these runs.',
  'limitations':['4x mode unimplemented; all factory values disable it.','Full signal equality verified at44100 only, MIDI24/velocity100/tempo120 and the recorded timing protocol.','Original instruction oracle imports current ARM libSystem math.','Actual DAW installation/session migration is not tested.']}
 (SUITE/'native/provenance.json').write_text(json.dumps(result,indent=2)+'\n')
 print('Verified 128 native stereo payloads,128 zero difference payloads,512 actual bundle comparisons and both bundle signatures.')
if __name__=='__main__':main()
