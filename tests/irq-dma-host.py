#!/usr/bin/env python3
"""Actual hostReadDma plus actual IRQ code against a threaded MMIO/scheduler model."""
import ast
from pathlib import Path
import subprocess, tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'NativeMsdc.cpp').read_text()
wait=s[s.index('constexpr uint32_t kDmaEvents'):s.index('\n#endif',s.index('constexpr uint32_t kDmaEvents'))]
start=s.index('IOReturn hostReadDma (');dma=s[start:s.index('\n// Bounded PIO write',start)]
mod=ast.parse((root/'tests/irq-wait-host.py').read_text())
pre=next(ast.literal_eval(n.value) for n in mod.body if isinstance(n,ast.Assign) and any(isinstance(t,ast.Name) and t.id=='prefix' for t in n.targets))
pre=pre.replace('#include <atomic>', '#include <atomic>\n#include <cstring>')
pre=pre.replace('constexpr unsigned kMsdcIntr=0, kMsdcIntEn=1;', '''constexpr unsigned kMsdcIntr=0, kMsdcIntEn=1,kMsdcCfg=2,kDmaSaHigh=3,kDmaSa=4,kDmaLength=5,kDmaCfg=6,kDmaCtrl=7,kSdcSts=8;
constexpr uint32_t kCfgPio=8,kDmaCfgChecksum=2,kDmaCfgActive=1,kDmaCtrlStart=1,kDmaCtrlStop=2,kDmaCtrlLastBuffer=1024,kDmaCtrlBurstMask=0x7000,kDmaCtrlDescriptor=256,kDmaCtrlBurst64=0x6000,kStsSdcBusy=1;
constexpr unsigned kSector=512,kMaxBlocks=128,kDmaDescriptorBytes=4096,kCmdSetBlockCount=23,kCmdReadMulti=18,kCmdReadSingle=17,kCmdStop=12;
constexpr uint64_t kCommandTimeoutNs=1000000000ULL;''')
pre=pre.replace('constexpr int kIOReturnSuccess=0, kIOReturnIOError=1, kIOReturnNotReady=2;', 'constexpr int kIOReturnSuccess=0, kIOReturnIOError=1, kIOReturnNotReady=2,kIOReturnBadArgument=3,kIOReturnTimeout=4;')
pre=pre.replace('volatile uint32_t irqFault=0; } gHost{&spin};', 'volatile uint32_t irqFault=0; uint8_t *dmaVirt=nullptr;uint64_t dmaPhys=0;bool poisoned=false,dmaEnabled=true; } gHost{&spin};')
pre=pre.replace('std::atomic<uint32_t> status{0}, enabled{0};', 'std::atomic<uint32_t> status{0}, enabled{0}, mmio[16]{}; bool stopFailed=false;uint32_t native_msdc_dma=0;const char *native_msdc_failure=nullptr;')
pre=pre.replace('return n==kMsdcIntr?status.load():enabled.load();', 'return n==kMsdcIntr?status.load():n==kMsdcIntEn?enabled.load():mmio[n].load();')
pre=pre.replace('else if(!stuckEnabled) enabled.store(v);', 'else if(n==kMsdcIntEn) {if(!stuckEnabled) enabled.store(v);}else mmio[n].store(v);')
more=r'''
struct DmaGpd{};struct DmaBd{};
bool dmaDescriptors(DmaGpd*,DmaBd*,uint64_t,uint64_t,uint64_t,unsigned){return true;}
void cacheCleanInvalidate(const void*,size_t){}
enum class Data{None,ReadMulti,ReadSingle};
IOReturn hostCommand(unsigned,uint32_t,Data,uint32_t *response){*response=0;return 0;}
IOReturn hostPrepareData(uint32_t){return 0;}
IOReturn waitClear(unsigned reg,uint32_t){return reg==kDmaCfg && stopFailed?kIOReturnNotReady:kIOReturnSuccess;}
bool irqEnabled=true;int preemption=0;bool ml_get_interrupts_enabled(){return irqEnabled;}int get_preemption_level(){return preemption;}
using thread_t=void*;thread_t gPublisherThread=reinterpret_cast<void*>(1);
thread_local thread_t activeThread=reinterpret_cast<void*>(1);thread_t gIoWorkerThread=reinterpret_cast<void*>(3);thread_t current_thread(){return activeThread;}
#define NATIVE_MSDC_BLOCK_DEVICES 1
#define NATIVE_MSDC_IRQ_WAIT 1
#define NATIVE_MSDC_WORKER_EXPERIMENT 1
#define IOLog(...) do {} while(0)
'''
post=r'''
void init(uint8_t *p){status=0;enabled=0;asserted=false;stopFailed=false;native_msdc_dma=0;native_msdc_failure=nullptr;gHost.irqReady=true;gHost.irqArmed=false;gHost.irqDelivered=false;gHost.irqStatus=0;gHost.irqSpurious=0;gHost.irqFault=0;gHost.poisoned=false;gHost.dmaEnabled=true;gHost.dmaVirt=p;gHost.dmaPhys=0x10000;for(auto &x:mmio)x=0;}
void waitAsserted(){auto end=steady_clock::now()+seconds(1);while(!asserted && steady_clock::now()<end)std::this_thread::yield();assert(asserted);}
int main(){uint8_t storage[8192]{},out[512]{};memset(storage+4096,0xa5,512);int result=-1;
 // Unapproved caller, wrong thread, disabled IRQ and nonzero preemption all poll.
 for(int mode=0;mode<4;mode++){init(storage);uint64_t attempts=native_msdc_irq_stats[1];status=kIntXferDone;
  activeThread=mode==1?reinterpret_cast<void*>(2):gPublisherThread;irqEnabled=mode!=2;preemption=mode==3?1:0;
  assert(hostReadDma(1,out,1,mode!=0)==0 && native_msdc_irq_stats[1]==attempts);}
 activeThread=gPublisherThread;irqEnabled=true;preemption=0;
 init(storage);std::thread t([&]{result=hostReadDma(1,out,1,true);});waitAsserted();status=kIntXferDone;dmaIrq(nullptr);t.join();assert(result==0 && out[0]==0xa5 && out[511]==0xa5 && (mmio[kDmaCtrl]&kDmaCtrlStop));
 init(storage);memset(out,0,512);std::thread u([&]{result=hostReadDma(1,out,1,true);});waitAsserted();status=kIntXferDone|(1u<<14);dmaIrq(nullptr);u.join();assert(result==kIOReturnIOError && out[0]==0 && !gHost.poisoned);
 init(storage);memset(out,0,512);std::thread v([&]{result=hostReadDma(1,out,1,true);});waitAsserted();std::this_thread::sleep_for(milliseconds(15));status=kIntXferDone;v.join();assert(result==0 && !gHost.irqReady && out[0]==0xa5);
 init(storage);memset(out,0,512);result=hostReadDma(1,out,1,true);assert(result==kIOReturnTimeout && !gHost.irqReady && out[0]==0 && (mmio[kDmaCtrl]&kDmaCtrlStop));
 // A late completion after timeout is not copied to the caller or accepted by the ISR.
 status=kIntXferDone;dmaIrq(nullptr);assert(out[0]==0 && gHost.irqStatus==0);
 init(storage);stopFailed=true;memset(out,0,512);std::thread w([&]{result=hostReadDma(1,out,1,true);});waitAsserted();status=kIntXferDone;dmaIrq(nullptr);w.join();assert(result==kIOReturnNotReady && gHost.poisoned && !gHost.dmaEnabled && native_msdc_dma==3 && out[0]==0);
 puts("PASS actual DMA: IRQ data copy, simultaneous error, absent-IRQ poll recovery, stop-failure quarantine");}
'''
with tempfile.TemporaryDirectory(prefix='msdc-dma-host-') as d:
 p=Path(d)/'a.cpp';x=Path(d)/'a';p.write_text(pre+more+wait+dma+post)
 subprocess.run(['c++','-std=c++17','-O2','-Wall','-Wextra','-Werror','-pthread',str(p),'-o',str(x)],check=True)
 subprocess.run([str(x)],check=True)
