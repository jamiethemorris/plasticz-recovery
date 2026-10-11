#!/usr/bin/env python3
"""Original effect dispatcher across live control changes; cached native replay.
Only input filter/output limiter calls are skipped via emulator-memory RET bytes.
All original effects, parameter-reset logic and note-on logic execute unchanged.
"""
from pathlib import Path
import hashlib,json,math,struct
from full_dsp_oracle import FullDSPOracle
from voice_oracle import OBJECT,OUT_POINTERS,OUT_LEFT,OUT_RIGHT
ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'analysis/recovered/effects-sequence'
# type, bypass, mix, p1, p2, tempo, frames, note-on trigger
EVENTS=[(0,0,.4,.2,.7,120,512,0),(2,0,.8,.2,.72,120,25000,0),
 (2,1,.8,.2,.72,120,2048,0),(2,0,.8,.2,.72,74,4096,0),
 (1,0,.5,.65,.5,74,12000,0),(1,1,.5,.65,.5,74,2048,0),
 (1,0,.9,.81,.2,180,12000,0),(6,0,.7,.3,.8,180,4096,0),
 (0,0,.7,.3,.8,180,1024,0),(6,0,.7,.3,.8,180,4096,0),
 (10,0,.8,.65,.4,180,4096,0),(9,0,.6,.3,.8,180,4096,0),
 (10,0,.8,.65,.4,180,4096,0),(5,0,.8,.71,.6,180,4096,0),
 (8,0,.7,.2,.4,180,4096,0),(7,0,.5,.8,.5,180,4096,0),
 (3,0,.7,.61,.3,120,10000,1),(3,1,.7,.61,.3,120,4096,0),
 (3,0,.7,.61,.3,120,10000,1),(3,0,.3,.12,.85,320,10000,0),
 (1,0,.8,.75,.6,120,20000,0),(1,1,.8,.75,.6,120,4096,0)]
BLOCKS=(512,44,1,127,7,0,256)
def main():
 OUT.mkdir(parents=True,exist_ok=True)
 p=json.loads((ROOT/'analysis/recovered/factory-presets.json').read_text())['programs'][0]['values']
 p[28]=0;p[35]=1;p[36]=0;p[37]=0
 o=FullDSPOracle();o.initialize(p)
 obj=struct.unpack('<I',o.uc.mem_read(OBJECT+0x40eb90,4))[0]
 vt=struct.unpack('<I',o.uc.mem_read(obj,4))[0]
 assert struct.unpack('<I',o.uc.mem_read(vt+0x4c,4))[0]==0x24c70
 o.uc.mem_write(0x24c70,b'\xc3');o.uc.mem_write(0x2d6aa,b'\xc3')
 data=bytearray(b'PZFS0001'+struct.pack('<I',len(EVENTS)));metadata=[];clock=0
 for event,(typ,bypass,mix,p1,p2,tempo,frames,trigger) in enumerate(EVENTS):
  p[28]=(typ+.2)/10.989999771118164;p[29:32]=[mix,p1,p2];p[36]=bypass
  p=list(struct.unpack('<38f',struct.pack('<38f',*p)))
  for index,value in enumerate(p):o.invoke(0x12f4a,struct.pack('<IIf',OBJECT,index,value),fpcw=0x037f)
  o.write_float(0x4006a0,tempo)
  if trigger:o.note_on(24,1.0)
  samples=[0.37*math.sin((clock+i)*.031)+0.11*math.sin((clock+i)*.00097) if i<frames*3//4 else 0.0 for i in range(frames)]
  clock+=frames;inp=struct.pack('<%df'%frames,*samples);left=bytearray();right=bytearray();at=bi=0
  while at<frames:
   n=min(BLOCKS[bi%len(BLOCKS)],frames-at);bi+=1
   o.uc.mem_write(OUT_LEFT,inp[at*4:(at+n)*4] or bytes(4));o.uc.mem_write(OUT_RIGHT,bytes(max(4,n*4)))
   o.invoke(0x10e4a,struct.pack('<IIII',OBJECT,OUT_POINTERS,0,n))
   left.extend(o.uc.mem_read(OUT_LEFT,n*4));right.extend(o.uc.mem_read(OUT_RIGHT,n*4));at+=n
  assert all(math.isfinite(x) for x in struct.unpack('<%df'%(frames*2),left+right))
  data.extend(struct.pack('<38ffII',*p,tempo,frames,trigger));data.extend(inp);data.extend(left);data.extend(right)
  metadata.append({'event':event,'type':typ,'bypass':bypass,'tempo':tempo,'frames':frames,'note_on':trigger,
                   'input_sha256':hashlib.sha256(inp).hexdigest(),'left_sha256':hashlib.sha256(left).hexdigest(),'right_sha256':hashlib.sha256(right).hexdigest()})
  print('event',event,'type',typ,'bypass',bypass,'complete',flush=True)
 (OUT/'sequence.bin').write_bytes(data)
 (OUT/'manifest.json').write_text(json.dumps({'protocol':'plasticz-effects-live-controls-v1','sample_rate':44100,'blocks':BLOCKS,
   'source_sha256':o.sha256,'fixture_sha256':hashlib.sha256(data).hexdigest(),
   'isolation':'RET substituted in emulator memory at0x24c70 and0x2d6aa. All original parameter setters and note-on calls execute.',
   'dependencies':{name:hashlib.sha256((ROOT/'scripts'/name).read_bytes()).hexdigest() for name in ('effects_sequence_oracle.py','full_dsp_oracle.py','voice_oracle.py','x86_oracle.py')},
   'events':metadata},indent=2)+'\n')
if __name__=='__main__':main()
