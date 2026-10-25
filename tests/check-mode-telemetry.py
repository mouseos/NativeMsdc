#!/usr/bin/env python3
"""Compile actual read-only mode logger/decoder with checked MMIO and lifetime mocks."""
from pathlib import Path
import subprocess,tempfile
R=Path(__file__).resolve().parents[1]
s=(R/'NativeMsdc.cpp').read_text()
def function(signature):
 a=s.index(signature);b=s.index('\n}',a)+2;return s[a:b]
logger=function('void hostLogMode (');boot=function('unsigned bootArgument (')
assert not any(x in logger for x in ('wr (','hostCommand (','hostRead','transfer (','cacheClean'))
start=function('bool NativeMsdc::start (')
assert 'gModeTelemetryEnabled = bootArgument ("native-msdc-mode", 1, 0) == 1;' in start
bringup=function('bool hostBringUp (')
assert bringup.index('hostReadPio (kCmdSendExtCsd')<bringup.index('hostLogMode (extCsd, inheritedCfg)')<bringup.index('if (useDma) hostEnableDma')
pre=r'''
#include <cassert>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include "NativeMsdc.h"
using namespace NativeMsdcHw;
static bool gModeTelemetryEnabled,present=true,failMemory,failMap,shortMap,zeroVa;
static unsigned flag=1,parses,reads,maps,descriptors,descriptorLive,mapLive,lines;
static uint32_t cfg=0x02000199,io=0x01010101,sdc=2u<<16,pad0=0x11223344,pad1=0xaabbccdd;
static const uint32_t mux=2u<<11;
static char output[2048];
static bool PE_parse_boot_argn(const char*n,void*out,size_t size){assert(!strcmp(n,"native-msdc-mode")&&size==sizeof(flag));parses++;memcpy(out,&flag,size);return present;}
static uint32_t rd(unsigned offset){reads++;switch(offset){case kMsdcCfg:return cfg;case kMsdcIoCon:return io;case kSdcCfg:return sdc;case kPadTune0:return pad0;case kPadTune1:return pad1;default:assert(false);return 0;}}
constexpr unsigned kIOMapAnywhere=1,kIOMapInhibitCache=2;
struct IOMemoryMap{IOMemoryMap(){mapLive++;}uintptr_t getVirtualAddress(){return zeroVa?0:(uintptr_t)&mux;}size_t getLength(){return shortMap?3:4096;}void release(){assert(mapLive);mapLive--;delete this;}};
struct IODeviceMemory{
 IODeviceMemory(){descriptorLive++;}
 static IODeviceMemory*withRange(uint64_t address,size_t length){assert(address==0x10000000ULL&&length==4096);descriptors++;return failMemory?nullptr:new IODeviceMemory;}
 IOMemoryMap*map(unsigned flags){assert(flags==(kIOMapAnywhere|kIOMapInhibitCache));maps++;return failMap?nullptr:new IOMemoryMap;}
 void release(){assert(descriptorLive);descriptorLive--;delete this;}
};
#define OSSafeReleaseNULL(x) do{if(x){x->release();x=nullptr;}}while(0)
static void IOLog(const char*format,...){
 assert(!mapLive&&!descriptorLive);char line[512];va_list a;va_start(a,format);int n=vsnprintf(line,sizeof(line),format,a);va_end(a);
 assert(n>0&&n<=200&&line[n-1]=='\n');assert(strlen(output)+n<sizeof(output));strcat(output,line);lines++;
}
'''
main=r'''
int main(){
 // Published log literal from batchUI, decoded only against register definitions.
 auto mode=decodeBusMode(0x02000199,2u<<16,1);
 assert(mode.clockMode==0&&mode.clockDivider==1&&mode.clockStable&&mode.hostBits==8&&!mode.hostDdr&&mode.cardTiming==1);
 for(unsigned host=0;host<4;host++)for(unsigned clk=0;clk<4;clk++){
  auto m=decodeBusMode((clk<<20)|(0xfffu<<8),host<<16,0xf3);
  assert(m.clockMode==clk&&m.clockDivider==4095&&m.hostDdr==(clk==2||clk==3)&&m.cardTiming==3);
  unsigned h=host==0?1:host==1?4:host==2?8:0;
  assert(m.hostBits==h);
 }
 uint8_t ext[512]{};ext[183]=2;ext[185]=1;ext[192]=8;ext[196]=0x57;ext[197]=0x1f;
 for(present=false;;present=true){
  for(flag=0;flag<4;flag++){
   parses=reads=maps=descriptors=lines=0;output[0]=0;
   gModeTelemetryEnabled=bootArgument("native-msdc-mode",1,0)==1;
   hostLogMode(ext,0x02000199);
   bool active=present&&flag==1;assert(parses==1&&reads==(active?5u:0u)&&maps==(active?1u:0u)&&descriptors==(active?1u:0u)&&lines==(active?4u:0u));
   assert(!mapLive&&!descriptorLive);
   if(active){assert(strstr(output,"ckmod=0 ckdiv=1 stable=1 bits=8 ddr=0"));assert(strstr(output,"bus_width_raw=02 bus_width=not_readable hs_timing=01 card_type=57"));assert(strstr(output,"mux_valid=1 mux_reg=00001000 msdc0_mux=2 source_hz=unmeasured"));}
  }
  if(present)break;
 }
 // Every possible unreadable byte must remain raw, never decoded as card mode.
 gModeTelemetryEnabled=true;
 for(unsigned raw=0;raw<256;raw++){
  ext[183]=raw;output[0]=0;hostLogMode(ext,0x02000199);
  assert(strstr(output,"bus_width=not_readable"));
  assert(!strstr(output,"width_match=")&&!strstr(output,"card_bits=")&&!strstr(output,"card_ddr="));
 }
 flag=~0u;assert(bootArgument("native-msdc-mode",1,0)==0);
 gModeTelemetryEnabled=true;
 for(unsigned failure=0;failure<4;failure++){
  failMemory=failure==0;failMap=failure==1;shortMap=failure==2;zeroVa=failure==3;
  reads=maps=descriptors=lines=0;output[0]=0;
  hostLogMode(ext,~0u);assert(reads==5&&lines==4&&!descriptorLive&&!mapLive&&strstr(output,"mux_valid=0"));
 }
 failMemory=failMap=shortMap=zeroVa=false;
 memset(ext,0xff,sizeof(ext));cfg=io=sdc=pad0=pad1=~0u;output[0]=0;lines=0;hostLogMode(ext,~0u);assert(lines==4);
 puts("PASS actual mode telemetry: default/strict gate, raw fields+host width/DDR decoding, unreadable card width never decoded, zero default MMIO/maps/logs, mapped-register address/attributes, failure cleanup, <=200-byte lines; no commands/writes/frequency inference");
}
'''
with tempfile.TemporaryDirectory(prefix='msdc-mode-') as directory:
 t=Path(directory);(t/'test.cpp').write_text(pre+boot+logger+main)
 subprocess.run(['c++','-std=c++17','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-pie','-no-pie','-I',str(R),str(t/'test.cpp'),'-o',str(t/'test')],check=True)
 subprocess.run([str(t/'test')],check=True)
