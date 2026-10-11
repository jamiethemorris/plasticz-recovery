#include "Effects.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
namespace {
void require(bool ok,const char* s){if(!ok)throw std::runtime_error(s);}
template<class T>T read(std::istream& f){T v{};f.read(reinterpret_cast<char*>(&v),sizeof(v));require(bool(f),"Truncated fixture");return v;}
std::uint32_t bits(float x){std::uint32_t n;std::memcpy(&n,&x,4);return n;}
std::vector<float> samples(std::istream& f,unsigned n){std::vector<float> v(n);f.read(reinterpret_cast<char*>(v.data()),n*4);require(bool(f),"Truncated samples");return v;}
}
int main(int argc,char**argv){try{
 require(argc==2,"Usage: check_effects sequence.bin");std::ifstream file(argv[1],std::ios::binary);char magic[8];file.read(magic,8);require(std::memcmp(magic,"PZFS0001",8)==0,"Wrong fixture header");
 auto cases=read<std::uint32_t>(file);require(cases==22,"Wrong case count");
 constexpr std::array<int,7> blocks{512,44,1,127,7,0,256};std::uint64_t total=0,mismatches=0;double maxError=0;
 plasticz::Effects effect;effect.prepare(44100);
 for(unsigned c=0;c<cases;++c){plasticz::Parameters p;for(float&v:p)v=read<float>(file);float tempo=read<float>(file);auto frames=read<std::uint32_t>(file);require(frames>0&&frames<=25000,"Wrong frame count");auto trigger=read<std::uint32_t>(file);
 auto input=samples(file,frames),expectedL=samples(file,frames),expectedR=samples(file,frames);std::vector<float> outputR(frames);
 effect.setParameters(p);effect.setTempo(tempo);if(trigger)effect.retriggerGate();
 unsigned at=0,block=0;while(at<frames){int n=std::min<unsigned>(blocks[block++%blocks.size()],frames-at);effect.process(input.data()+at,outputR.data()+at,n);at+=n;}
 std::uint64_t bad=0;int first=-1;for(unsigned i=0;i<frames;++i){for(unsigned ch=0;ch<2;++ch){float a=ch?outputR[i]:input[i],e=ch?expectedR[i]:expectedL[i];require(std::isfinite(a),"Nonfinite effect output");if(bits(a)!=bits(e)){++bad;if(first<0)first=i;maxError=std::max(maxError,std::abs(double(a)-e));}++total;}}
 if(bad)std::cerr<<"case="<<c<<" type="<<int(p[28]*10.989999771118164f)<<" bits="<<bad<<" first="<<first<<"\n";mismatches+=bad;
 }
 require(file.peek()==std::char_traits<char>::eof(),"Unexpected trailing fixture bytes");std::cout<<"Compared "<<total<<" live-effect channel samples, mismatches="<<mismatches<<" max_error="<<maxError<<"\n";return mismatches?1:0;
 }catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 2;}}
