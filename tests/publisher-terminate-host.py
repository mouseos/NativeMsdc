#!/usr/bin/env python3
"""Actual stop, cleanup worker and child start against a modeled IOKit lifecycle."""
from pathlib import Path
import subprocess,tempfile
s=(Path(__file__).resolve().parents[1]/'NativeMsdc.cpp').read_text()
def extract(start,end):
 a=s.index(start);b=s.index(end,a);return s[a:b]
child=extract('bool NativeMsdcBlock::start (IOService *provider)','\nIOReturn NativeMsdcBlock::doSynchronizeCache')
cleanup=extract('void publisherCleanupThread (void *argument, wait_result_t)','\n// Ordered publication:')
stop=extract('void NativeMsdc::stop (IOService *provider)','\n// ---- Persistent store')
root=extract('bool NativeMsdc::startRoot (bool allowWrites, const char **why)','\n#endif // NATIVE_MSDC_BLOCK_DEVICES')
assert root.index('kernel_thread_start (publisherCleanupThread') < root.index('kernel_thread_start (publishThread')
assert 'compare_exchange_n (&gPublisherCleanup' in root and 'thread_wakeup' in root
pre=r'''
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
using wait_result_t=int; using event_t=void*; using thread_t=void*;
constexpr int THREAD_UNINT=0,THREAD_WAITING=1,THREAD_AWAKENED=2,kIOServiceRequired=1;
#define NATIVE_MSDC_BLOCK_DEVICES 1
#define NATIVE_MSDC_IRQ_WAIT 0
#define IOLog(...) do {} while(0)
bool gPublisherStopped=false;uint32_t gPublisherCleanup=0;uint32_t native_msdc_publisher_terminate_failed=0;
std::mutex events,arb;std::condition_variable cv;bool signaled=false;std::atomic<int> attempts{0};int failAttempts=0;
int assert_wait(event_t,int){return THREAD_WAITING;}thread_t current_thread(){return nullptr;}
int clear_wait(thread_t,int){return 0;}void thread_wakeup(event_t){std::lock_guard<std::mutex> l(events);signaled=true;cv.notify_all();}
#define THREAD_CONTINUE_NULL nullptr
int thread_block(void*){std::unique_lock<std::mutex> l(events);cv.wait(l,[]{return signaled;});signaled=false;return 0;}
void IOSleep(unsigned){std::this_thread::yield();}
extern std::atomic<bool> parentInactive;
struct IOService {virtual bool isInactive(){return parentInactive;} virtual void stop(IOService*){} virtual ~IOService()=default;};
struct NativeMsdcBlock;
std::atomic<bool> parentInactive{false},childInactive{false},entered{false},go{false};int matched=0;bool gate=false;
struct NativeMsdc:IOService {std::atomic<int> refs{1};bool isInactive() override{return parentInactive;}bool terminate(int){if(attempts++<failAttempts)return false;std::lock_guard<std::mutex> l(arb);parentInactive=true;childInactive=true;return true;}void retain(){++refs;}void release(){--refs;}void stop(IOService*) override;};
struct IOBlockStorageDevice:IOService {bool isInactive() override{return childInactive;}bool start(IOService*){return true;}};
struct NativeMsdcBlock:IOBlockStorageDevice {bool online=false;bool start(IOService*);void registerService(){entered=true;while(gate&&!go)std::this_thread::yield();std::lock_guard<std::mutex> l(arb);if(!childInactive)++matched;}};
'''
post=r'''
int main(){NativeMsdc owner;IOService provider;NativeMsdcBlock child;owner.retain();failAttempts=1;std::thread cleanup([&]{publisherCleanupThread(&owner,0);});
 gate=true;std::thread pub([&]{assert(child.start(&owner));});while(!entered)std::this_thread::yield();owner.stop(&provider);
 while(!childInactive){std::this_thread::yield();} assert(parentInactive);go=true;pub.join();cleanup.join();assert(!matched&&attempts==2&&owner.refs==1);
 parentInactive=false;childInactive=false;gPublisherStopped=false;gPublisherCleanup=0;gate=false;matched=0;attempts=0;failAttempts=0;
 NativeMsdcBlock prior;assert(prior.start(&owner)&&matched==1);owner.stop(&provider);owner.retain();publisherCleanupThread(&owner,0);
 assert(parentInactive&&childInactive&&owner.refs==1&&attempts==1);
 parentInactive=false;childInactive=false;gPublisherCleanup=2;owner.retain();publisherCleanupThread(&owner,0);assert(!parentInactive&&owner.refs==1);
 parentInactive=false;childInactive=false;gPublisherCleanup=0;gPublisherStopped=false;failAttempts=0;attempts=0;
 owner.stop(&provider);owner.retain();publisherCleanupThread(&owner,0);assert(parentInactive&&childInactive&&owner.refs==1&&attempts==1);
 // Reverse ordering: a direct stop does not immediately make children inactive.
 parentInactive=false;childInactive=false;gPublisherStopped=false;gPublisherCleanup=0;gate=true;go=false;entered=false;matched=0;
 NativeMsdcBlock late;std::thread direct([&]{assert(late.start(&owner));});while(!entered)std::this_thread::yield();
 owner.stop(&provider);go=true;direct.join();assert(matched==1 && !parentInactive);
 owner.retain();publisherCleanupThread(&owner,0);assert(parentInactive && childInactive && owner.refs==1);
 // Permanent failure is bounded and visible; no false inactive state.
 parentInactive=false;childInactive=false;gPublisherStopped=false;gPublisherCleanup=0;native_msdc_publisher_terminate_failed=0;attempts=0;failAttempts=100;
 owner.stop(&provider);owner.retain();publisherCleanupThread(&owner,0);
 assert(attempts==8 && native_msdc_publisher_terminate_failed==1 && !parentInactive && owner.refs==1);
 // Normal single-provider termination marks the parent and child inactive before stop.
 parentInactive=true;childInactive=true;gPublisherStopped=false;gPublisherCleanup=0;attempts=0;failAttempts=0;matched=0;
 owner.stop(&provider);NativeMsdcBlock inactive;assert(!inactive.start(&owner)&&!matched);
 owner.retain();publisherCleanupThread(&owner,0);assert(attempts==0&&owner.refs==1);
 puts("PASS modeled lifecycle, direct-stop reverse race, eventual cleanup, bounded failure and lifetime");}
'''
with tempfile.TemporaryDirectory(prefix='publisher-terminate-') as d:
 p=Path(d)/'test.cpp';x=Path(d)/'test';p.write_text(pre+child+cleanup+stop+post)
 subprocess.run(['c++','-std=c++17','-pthread','-Wall','-Wextra','-Werror',str(p),'-o',str(x)],check=True)
 subprocess.run([str(x)],check=True,timeout=10)
