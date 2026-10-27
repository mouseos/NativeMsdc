#!/usr/bin/env python3
"""Run the actual IRQ wait and IRQ handler with a threaded register/scheduler model."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
src = (root / 'NativeMsdc.cpp').read_text()
start = src.index('constexpr uint32_t kDmaEvents')
end = src.index('\n#endif', start)
actual = src[start:end]
assert 'if (result == kIOReturnNotReady)' in src
assert 'else if (gHost.dmaEnabled) result = hostReadDma' in src
assert 'else result = hostReadPio' in src
prefix = r'''
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>
#include <time.h>
using namespace std::chrono;
constexpr uint32_t kIntXferDone=1u<<12, kIntDataErrors=3u<<14, kIntDmaErrors=7u<<17;
constexpr unsigned kMsdcIntr=0, kMsdcIntEn=1;
constexpr int kIOReturnSuccess=0, kIOReturnIOError=1, kIOReturnNotReady=2;
using IOReturn=int; using IOInterruptState=int; using wait_result_t=int; using event_t=void*;
constexpr int THREAD_UNINT=0, THREAD_WAITING=1; constexpr void *THREAD_CONTINUE_NULL=nullptr;
std::mutex spin, sleepMutex; std::condition_variable cv; unsigned epoch=0;
thread_local unsigned myEpoch; thread_local uint64_t myDeadline;
uint64_t native_msdc_irq_stats[4]{}, native_msdc_irq_mask_failures=0;
bool maskWorks=true,stuckEnabled=false;int panicCount=0;void panic(const char*){++panicCount;}
void absolutetime_to_nanoseconds(uint64_t in,uint64_t *out){*out=in;}
std::atomic<bool> asserted{false};
std::atomic<uint32_t> status{0}, enabled{0};
struct Host { std::mutex *irqLock; bool irqArmed=false, irqReady=true, irqDelivered=false; uint32_t irqStatus=0,irqSpi=78,irqSpurious=0; volatile uint32_t irqFault=0; } gHost{&spin};
bool armcpu_spi_mask_owned(uint32_t id,void(*)(void*),void*){assert(id==110);return maskWorks;}
uint64_t mach_absolute_time(){return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();}
uint64_t deadlineAfter(uint64_t ns){return mach_absolute_time()+ns;}
int IOSimpleLockLockDisableInterrupt(std::mutex *m){m->lock();return 1;}
void IOSimpleLockUnlockEnableInterrupt(std::mutex *m,int){m->unlock();}
uint32_t rd(unsigned n){return n==kMsdcIntr?status.load():enabled.load();}
void wr(unsigned n,uint32_t v){if(n==kMsdcIntr)status.fetch_and(~v);else if(!stuckEnabled) enabled.store(v);}
int assert_wait_deadline(event_t,int,uint64_t d){std::lock_guard<std::mutex> l(sleepMutex);myEpoch=epoch;myDeadline=d;asserted=true;return THREAD_WAITING;}
int thread_block(void*){std::unique_lock<std::mutex> l(sleepMutex);cv.wait_until(l,steady_clock::time_point(nanoseconds(myDeadline)),[]{return epoch!=myEpoch;});return 0;}
void thread_wakeup(event_t){std::lock_guard<std::mutex> l(sleepMutex);++epoch;cv.notify_all();}
'''
suffix = r'''
void reset(){status=0;enabled=0;asserted=false;gHost.irqArmed=false;gHost.irqReady=true;gHost.irqStatus=0;gHost.irqSpurious=0;gHost.irqFault=0;maskWorks=true;stuckEnabled=false;panicCount=0;native_msdc_irq_mask_failures=0;}
void inject(uint32_t bits){status.fetch_or(bits);dmaIrq(nullptr);}
void waitAsserted(){auto end=steady_clock::now()+seconds(1);while(!asserted && steady_clock::now()<end)std::this_thread::yield();assert(asserted);}
uint64_t cpuNs(){timespec ts{};clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&ts);return uint64_t(ts.tv_sec)*1000000000+ts.tv_nsec;}
int main(){
 reset();status=kIntXferDone;assert(waitDmaIrq(deadlineAfter(100000000))==0);assert(status==0 && enabled==0);
 reset();int result=-1;std::thread t([&]{result=waitDmaIrq(deadlineAfter(100000000));});waitAsserted();inject(kIntXferDone);t.join();assert(result==0 && status==0 && !gHost.irqArmed);
 // Repeated spurious entries mask the owned SPI and wake the waiter.
 reset();result=-1;std::thread storm([&]{result=waitDmaIrq(deadlineAfter(100000000));});waitAsserted();for(int i=0;i<8;i++)dmaIrq(nullptr);storm.join();assert(result==kIOReturnNotReady && gHost.irqFault==1 && enabled==0);
 // GIC mask readback failure disables INTEN, records fault, wakes the waiter,
 // and escalates if further interrupts prove the source is not quiescent.
 reset();maskWorks=false;result=-1;std::thread badmask([&]{result=waitDmaIrq(deadlineAfter(100000000));});waitAsserted();for(int i=0;i<8;i++)dmaIrq(nullptr);badmask.join();assert(result==kIOReturnNotReady && gHost.irqFault==1 && native_msdc_irq_mask_failures==1 && enabled==0);
 for(int i=0;i<8;i++){dmaIrq(nullptr);}assert(panicCount==1);
 // A source that also refuses INTEN clear is immediately fatal.
 reset();maskWorks=false;stuckEnabled=true;enabled=1;gHost.irqSpurious=7;dmaIrq(nullptr);assert(panicCount==1 && native_msdc_irq_mask_failures==1);stuckEnabled=false;
 // IRQ has latched and W1C-cleared status before the waiter resumes: no lost wake.
 reset();result=-1;std::thread u([&]{result=waitDmaIrq(deadlineAfter(100000000));});waitAsserted();inject(kIntXferDone | (1u<<14));u.join();assert(result==kIOReturnIOError && status==0);
 reset();result=-1;std::thread v([&]{result=waitDmaIrq(deadlineAfter(100000000));});waitAsserted();inject(1u<<18);v.join();assert(result==kIOReturnIOError);
 reset();auto before=steady_clock::now();assert(waitDmaIrq(deadlineAfter(100000000))==kIOReturnNotReady);assert(!gHost.irqReady && enabled==0);assert(steady_clock::now()-before<milliseconds(200));
 // Status without IRQ must still complete once, then disable future IRQ waits.
 reset();result=-1;std::thread missed([&]{result=waitDmaIrq(deadlineAfter(100000000));});waitAsserted();status=kIntXferDone;missed.join();assert(result==0 && !gHost.irqReady && status==0);
 // Unrelated status is neither consumed nor treated as a DMA completion.
 reset();status=1u<<8;assert(waitDmaIrq(deadlineAfter(100000000))==kIOReturnNotReady);assert(status==(1u<<8));
 // Compare actual sleep path to a synthetic busy poll, same 5ms device delay.
 uint64_t sleepCpu=cpuNs();auto sleepWall=steady_clock::now();
 for(int i=0;i<20;i++){reset();result=-1;std::thread worker([&]{result=waitDmaIrq(deadlineAfter(100000000));});waitAsserted();std::this_thread::sleep_for(milliseconds(5));inject(kIntXferDone);worker.join();assert(result==0);}
 sleepCpu=cpuNs()-sleepCpu; auto sleepElapsed=duration_cast<microseconds>(steady_clock::now()-sleepWall).count();
 uint64_t busyCpu=cpuNs();auto busyWall=steady_clock::now();
 for(int i=0;i<20;i++){auto end=steady_clock::now()+milliseconds(5);while(steady_clock::now()<end)std::atomic_signal_fence(std::memory_order_seq_cst);}
 busyCpu=cpuNs()-busyCpu;auto busyElapsed=duration_cast<microseconds>(steady_clock::now()-busyWall).count();
 printf("PASS actual IRQ wait: early, wake, W1C+error, DMA error, timeout, unrelated; synthetic sleep CPU=%llu us wall=%lld us; busy CPU=%llu us wall=%lld us\n",(unsigned long long)(sleepCpu/1000),(long long)sleepElapsed,(unsigned long long)(busyCpu/1000),(long long)busyElapsed);
 assert(sleepCpu<busyCpu);
}
'''
with tempfile.TemporaryDirectory(prefix='msdc-irq-host-') as tmp:
    cpp = Path(tmp) / 'test.cpp'; exe = Path(tmp) / 'test'
    cpp.write_text(prefix + actual + suffix)
    subprocess.run(['c++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-pthread', str(cpp), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
