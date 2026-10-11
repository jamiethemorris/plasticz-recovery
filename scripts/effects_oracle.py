#!/usr/bin/env python3
"""Export isolated effect cases from original instructions, with filter/limiter skipped.

Only the two outer processing entry points are replaced by RET in emulator memory.
The effects dispatcher, helper, and reverb instructions remain unchanged. No file
or original installation is modified. Uses the frozen full oracle at 44100 Hz.
"""
from pathlib import Path
import hashlib, json, math, struct, sys
from full_dsp_oracle import FullDSPOracle
from voice_oracle import OBJECT, OUT_POINTERS, OUT_LEFT, OUT_RIGHT

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'analysis/recovered/effects'
BLOCKS=(1,7,44,512,127,0,31)

def main():
    OUT.mkdir(parents=True, exist_ok=True)
    factory=json.loads((ROOT/'analysis/recovered/factory-presets.json').read_text())['programs']
    oracle=FullDSPOracle()
    source=(ROOT/'analysis/source/PlastiCZ-i386').read_bytes()
    metadata=[]
    data=bytearray(b'PZFX0001'+struct.pack('<I',44))
    # Wet/mid/endpoints, independent rate/depth controls, and varying host tempos.
    settings=((0.0,0.0,1.0,120.0),(1.0,1.0,0.0,60.0),
              (0.43,0.27,0.71,137.0),(1.0,0.61,0.84,999.0))
    for effect in range(11):
        for setting,(mix,p1,p2,tempo) in enumerate(settings):
            p=list(factory[0]['values']); p[28]=effect/10.989999771118164+0.001
            if effect==10: p[28]=1.0
            p[29:32]=[mix,p1,p2];p[36]=0;p[37]=0
            p=list(struct.unpack('<38f',struct.pack('<38f',*p)))
            oracle.initialize(p)
            oracle.write_float(0x4006a0,tempo)
            filter_object=struct.unpack('<I',oracle.uc.mem_read(OBJECT+0x40eb90,4))[0]
            vtable=struct.unpack('<I',oracle.uc.mem_read(filter_object,4))[0]
            filter_process=struct.unpack('<I',oracle.uc.mem_read(vtable+0x4c,4))[0]
            assert filter_process==0x24c70
            oracle.uc.mem_write(filter_process,b'\xc3')
            oracle.uc.mem_write(0x2d6aa,b'\xc3')
            frames=22050
            samples=[(0.45*math.sin(i*0.037)+0.23*math.sin(i*0.0031)) if i<11025 else 0.0 for i in range(frames)]
            samples[0]=1.0;samples[113]=-1.75;samples[449]=1e-38
            inputs=struct.pack('<%df'%frames,*samples)
            left,right=bytearray(),bytearray()
            position=block_index=0
            while position<frames:
                count=min(BLOCKS[block_index%len(BLOCKS)],frames-position);block_index+=1
                oracle.uc.mem_write(OUT_LEFT,inputs[position*4:(position+count)*4] or b'\0\0\0\0')
                oracle.uc.mem_write(OUT_RIGHT,bytes(max(4,count*4)))
                oracle.invoke(0x10e4a,struct.pack('<IIII',OBJECT,OUT_POINTERS,0,count))
                left.extend(oracle.uc.mem_read(OUT_LEFT,count*4));right.extend(oracle.uc.mem_read(OUT_RIGHT,count*4));position+=count
            values=struct.unpack('<%df'%(frames*2),left+right)
            assert all(math.isfinite(v) for v in values)
            data.extend(struct.pack('<38ffI',*p,tempo,frames));data.extend(inputs);data.extend(left);data.extend(right)
            metadata.append({'effect':effect,'setting':setting,'tempo':tempo,'frames':frames,'parameters':p,
                             'input_sha256':hashlib.sha256(inputs).hexdigest(),'left_sha256':hashlib.sha256(left).hexdigest(),
                             'right_sha256':hashlib.sha256(right).hexdigest()})
            print('effect',effect,'setting',setting,'complete',flush=True)
    path=OUT/'cases.bin';path.write_bytes(data)
    manifest={'protocol':'plasticz-isolated-effects-v1','source_sha256':hashlib.sha256(source).hexdigest(),
              'fixture_sha256':hashlib.sha256(data).hexdigest(),'sample_rate':44100,'blocks':BLOCKS,
              'isolation':'RET substituted in emulator memory at input filter 0x24c70 and output limiter 0x2d6aa only.',
              'coverage':'11 selectors, four parameter/tempo configurations each; arbitrary common mono input, mixed blocks and tail.',
              'cases':metadata,'dependencies':{name:hashlib.sha256((ROOT/'scripts'/name).read_bytes()).hexdigest()
                        for name in ('effects_oracle.py','full_dsp_oracle.py','voice_oracle.py','x86_oracle.py')}}
    (OUT/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    print('Saved',len(metadata),'cases,',len(data),'bytes')
if __name__=='__main__': main()
