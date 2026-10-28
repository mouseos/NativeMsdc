#!/usr/bin/env python3
"""Exercise the actual deferred activation thread against concurrent stop."""
from pathlib import Path
import subprocess, tempfile
s = (Path(__file__).resolve().parents[1] / 'NativeMsdc.cpp').read_text()
a = s.index('IOLock *gActivationLock = nullptr;')
b = s.index('\n}\n#endif\n\nbool NativeMsdc::start', a)
body = s[a:b]
pre = r'''
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>
using namespace std::chrono_literals;
using wait_result_t=int; using thread_t=std::thread*;
constexpr int KERN_SUCCESS=0;
#define NATIVE_MSDC_BLOCK_DEVICES 1
#define NATIVE_MSDC_WORKER_EXPERIMENT 1
#define IOLog(...) do {} while(0)
struct IOLock { std::mutex mutex; } lifecycle;
void IOLockLock(IOLock*l){l->mutex.lock();} void IOLockUnlock(IOLock*l){l->mutex.unlock();}
struct IOService {std::atomic<int> refs{1}; void retain(){++refs;} void release(){--refs;}};
struct NativeMsdc:IOService{};
struct {bool irqReady=false;} gHost;
std::atomic<bool> ready{false}, entering{false}, proceed{false};
std::atomic<int> attaches{0},workers{0},sleeps{0}; bool attachSuccess=true, blockAttach=false;
bool armcpu_spi_boot_ready(){return ready.load();}
void IOSleep(unsigned){++sleeps; std::this_thread::sleep_for(1ms);}
void hostEnableIrq(IOService*){++attaches; entering=true; while(blockAttach && !proceed) std::this_thread::yield(); __atomic_store_n(&gHost.irqReady,attachSuccess,__ATOMIC_RELEASE);}
bool startBlockWorker(NativeMsdc*){++workers;return true;}
void* IOMalloc(size_t n){return malloc(n);} void IOFree(void*p,size_t){free(p);}
std::vector<std::thread*> threads;
int kernel_thread_start(void(*fn)(void*,wait_result_t),void*arg,thread_t*out){*out=new std::thread(fn,arg,0);threads.push_back(*out);return KERN_SUCCESS;}
void thread_deallocate(thread_t){}
'''
post = r'''
void join(){for(auto*t:threads){t->join();delete t;}threads.clear();}
void reset(){gActivationLock=&lifecycle;gActivationStopped=false;ready=false;entering=false;proceed=false;attaches=0;workers=0;sleeps=0;blockAttach=false;attachSuccess=true;gHost.irqReady=false;}
void stop(){IOLockLock(gActivationLock);__atomic_store_n(&gActivationStopped,true,__ATOMIC_RELEASE);IOLockUnlock(gActivationLock);}
int main(){NativeMsdc owner;IOService provider;
 reset();scheduleIrqActivation(&owner,&provider,true);while(sleeps<2)std::this_thread::yield();assert(attaches==0&&workers==0);ready=true;join();assert(attaches==1&&workers==1&&owner.refs==1&&provider.refs==1);
 reset();scheduleIrqActivation(&owner,&provider,true);while(sleeps<2)std::this_thread::yield();stop();ready=true;join();assert(attaches==0&&workers==0&&owner.refs==1&&provider.refs==1);
 reset();scheduleIrqActivation(&owner,&provider,true);join();assert(attaches==0&&workers==0&&sleeps==600&&owner.refs==1&&provider.refs==1);
 reset();ready=true;attachSuccess=false;scheduleIrqActivation(&owner,&provider,true);join();assert(attaches==1&&workers==0);
 reset();ready=true;blockAttach=true;scheduleIrqActivation(&owner,&provider,true);while(!entering)std::this_thread::yield();std::atomic<bool> stopped{false};std::thread stopper([&]{stop();stopped=true;});std::this_thread::sleep_for(3ms);assert(!stopped);proceed=true;stopper.join();join();assert(attaches==1&&workers==1&&stopped&&owner.refs==1&&provider.refs==1);
 reset();ready=true;scheduleIrqActivation(&owner,&provider,false);join();assert(attaches==1&&workers==0);
 puts("PASS deferred readiness, single attach, failure, stop race, worker selection, retained lifetimes");}
'''
with tempfile.TemporaryDirectory(prefix='msdc-activation-') as d:
 p=Path(d)/'test.cpp'; exe=Path(d)/'test';p.write_text(pre+body+post)
 subprocess.run(['c++','-std=c++17','-pthread','-Wall','-Wextra','-Werror',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
