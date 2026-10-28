#!/usr/bin/env python3
"""Exercise actual IRQ setup failure/rollback using modeled IOKit objects."""
from pathlib import Path
import subprocess, tempfile
s=(Path(__file__).resolve().parents[1]/'NativeMsdc.cpp').read_text()
start=s.index('void hostEnableIrq (IOService *provider, HostSession &session)');end=s.index('\n#endif',start)
body=s[start:end].replace('__asm__ volatile ("dsb sy" ::: "memory");','__atomic_thread_fence (__ATOMIC_SEQ_CST);')
pre=r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
using IOReturn=int;constexpr int kIOReturnSuccess=0;
struct OSData { unsigned length; void *ptr; unsigned getLength(){return length;} void *getBytesNoCopy(){return ptr;} };
struct IOService {OSData *spi,*base,*low;OSData *getProperty(const char*n){return !strcmp(n,"msdc-irq-spi")?spi:!strcmp(n,"msdc-sysirq-base")?base:low;}};
#define OSDynamicCast(type,obj) (obj)
uint32_t regs[1024]{};bool failMap=false,failLock=false;int attachResult=0;uint32_t attachReadback=0xa003;uint64_t attachTarget=1;unsigned attaches=0;
struct IOMemoryMap {uintptr_t getVirtualAddress(){return reinterpret_cast<uintptr_t>(regs);} void release(){}} map;
struct IODeviceMemory {static IODeviceMemory* withRange(uint64_t,uint64_t);IOMemoryMap* map(unsigned){return failMap?nullptr:&::map;}void release(){}} mem;
IODeviceMemory* IODeviceMemory::withRange(uint64_t,uint64_t){return &mem;}
constexpr unsigned kIOMapAnywhere=1,kIOMapInhibitCache=2;
#define OSSafeReleaseNULL(x) do { if(x){(x)->release();(x)=nullptr;} } while(0)
struct IOSimpleLock{} lock;
IOSimpleLock* IOSimpleLockAlloc(){return failLock?nullptr:&lock;}void IOSimpleLockFree(IOSimpleLock*){}
struct Host {bool dmaEnabled=true,irqReady=false;uint32_t irqSpi=0;volatile uint32_t irqFault=0;IOSimpleLock*irqLock=nullptr;IOMemoryMap*sysirqMap=nullptr;} gHost;
uint32_t inherited=0;struct HostSession {uint32_t inheritedInterrupts(){return inherited;}};
struct IOLock{} gate; IOLock *gActivationLock=&gate; bool gActivationStopped=false; void IOLockLock(IOLock*){} void IOLockUnlock(IOLock*){}
void dmaIrq(void*){}
IOReturn armcpu_spi_attach_sysirq(uint32_t spi,volatile uint32_t *pol,void(*)(void*),void*,uint32_t*readback,uint64_t*target,bool*installed){assert(spi==78);++attaches;*readback=attachReadback;*target=attachTarget;*installed=attachResult==2;if(!attachResult){*pol|=1u<<14;*installed=true;}return attachResult;}
#define IOLog(...) do {} while(0)
'''
post=r'''
int main(){HostSession session;uint32_t spi=78,low=1;uint64_t base=0x10200620;
 OSData a{4,&spi},b{8,&base},c{4,&low};IOService provider{&a,&b,&c};
 inherited=1;hostEnableIrq(&provider,session);assert(attaches==0);
 inherited=0;low=0;hostEnableIrq(&provider,session);assert(attaches==0);low=1;
 failMap=true;hostEnableIrq(&provider,session);assert(!gHost.irqLock && !gHost.sysirqMap);failMap=false;
 unsigned idx=(0x620+8)/4;uint32_t bit=1u<<14;regs[idx]=0x100;attachResult=1;
 hostEnableIrq(&provider,session);assert(attaches==1 && regs[idx]==0x100 && !gHost.irqLock);
 attachResult=2;hostEnableIrq(&provider,session);assert(!gHost.irqReady && gHost.irqLock && !(regs[idx]&bit));
 gHost={};gHost.dmaEnabled=true;attachResult=0;attachReadback=0xa003;hostEnableIrq(&provider,session);assert(gHost.irqReady && (regs[idx]&bit));
 puts("PASS actual IRQ setup: metadata, inherited mask, map failure, rollback, readback refusal, valid GIC target");}
'''
with tempfile.TemporaryDirectory(prefix='msdc-setup-') as d:
 p=Path(d)/'a.cpp';x=Path(d)/'a';p.write_text(pre+body+post)
 subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror',str(p),'-o',str(x)],check=True)
 subprocess.run([str(x)],check=True)
