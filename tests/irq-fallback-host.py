#!/usr/bin/env python3
"""Compile the actual DMA/PIO dispatch with modeled media operations."""
from pathlib import Path
import subprocess, tempfile
src=(Path(__file__).resolve().parents[1]/'NativeMsdc.cpp').read_text()
transfer=src[src.index('IOReturn transfer ('):src.index('\nunsigned bootArgument',src.index('IOReturn transfer ('))]
pre=r'''
#include <cstdint>
#include <cassert>
#include <cstdio>
using IOReturn=int; constexpr int kIOReturnNotReady=1,kIOReturnBadArgument=2,kIOReturnSuccess=0,kIOReturnIOError=3;
constexpr unsigned kMaxBlocks=128,kCmdReadSingle=17;
struct Host { bool poisoned=false,dmaEnabled=false; uint32_t diskSectors=1000; } gHost;
uint64_t native_msdc_failures=0,native_msdc_writes=0,native_msdc_reads=0;
int pio=0,dma=0,writes=0; int next=0;
int hostReadPio(unsigned,uint32_t,uint8_t*,uint32_t){++pio;return next;}
int hostReadDma(uint32_t,uint8_t*,uint32_t){++dma;return next;}
int hostWrite(uint32_t,uint8_t*,uint32_t){++writes;return next;}
'''
post=r'''
int main(){uint8_t b[512]{}; assert(transfer(0,1,b,false)==0 && pio==1 && dma==0);
 gHost.dmaEnabled=true;assert(transfer(1,1,b,false)==0 && dma==1);
 next=kIOReturnIOError;assert(transfer(1,1,b,false)==kIOReturnIOError && pio==1 && dma==2);
 gHost.dmaEnabled=false;next=0;assert(transfer(2,1,b,false)==0 && pio==2);
 assert(transfer(2,1,b,true)==0 && writes==1);gHost.poisoned=true;
 assert(transfer(2,1,b,false)==kIOReturnNotReady && pio==2 && dma==2);
 puts("PASS actual transfer dispatch: PIO fallback, DMA, error propagation, writes and poison");}
'''
with tempfile.TemporaryDirectory(prefix='msdc-fallback-') as d:
 p=Path(d)/'a.cpp';x=Path(d)/'a';p.write_text(pre+transfer+post)
 subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror',str(p),'-o',str(x)],check=True)
 subprocess.run([str(x)],check=True)
