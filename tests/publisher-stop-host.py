#!/usr/bin/env python3
"""Exercise cancellation and document the in-flight start race; not a safety proof."""
from pathlib import Path
import subprocess,tempfile
s=(Path(__file__).resolve().parents[1]/'NativeMsdc.cpp').read_text()
a=s.index('void publishThread (void *, wait_result_t)');b=s.index('\n}\n\n} // namespace',a)+2
body=s[a:b]
pre=r'''
#include <atomic>
#include <cassert>
#include <cstdio>
#include <thread>
using wait_result_t=int;using thread_t=int;
#define NATIVE_MSDC_IRQ_WAIT 0
#define NATIVE_MSDC_VERIFIED 0
#define IOLog(...) do {} while(0)
bool gPublisherStopped=false,gVerifyWindows=false;unsigned native_msdc_blocks_published=0,native_msdc_step=0;
const char *native_msdc_failure=nullptr;
struct Provider{void release(){}} provider,*gPendingProvider=&provider;
std::atomic<bool> inside{false}, proceed{false};bool block=false;
struct NativeMsdcBlock {int starts=0,refs=1; bool start(Provider*){++starts;inside=true;while(block&&!proceed)std::this_thread::yield();return true;}void release(){--refs;}const char*windowLabel(){return "mock";}} device;
NativeMsdcBlock *gPending[1]={&device};struct {unsigned count=1,order[1]={0};} gPlan;
struct {struct {unsigned count=1;} manifest;} gHost;
bool windowMatchesManifest(NativeMsdcBlock*){return true;}
bool volumeBelow(NativeMsdcBlock*){return true;}void IOSleep(unsigned){}
void discardPending(unsigned i){if(gPending[i]){gPending[i]->release();gPending[i]=nullptr;}}
'''
post=r'''
int main(){gPublisherStopped=true;publishThread(nullptr,0);assert(device.starts==0&&!gPending[0]);
 gPending[0]=&device;device.refs=1;gPublisherStopped=false;block=true;std::thread t([]{publishThread(nullptr,0);});
 while(!inside){std::this_thread::yield();} __atomic_store_n(&gPublisherStopped,true,__ATOMIC_RELEASE);proceed=true;t.join();
 assert(device.starts==1&&native_msdc_blocks_published==1);
 puts("PASS cooperative publisher cancellation; reproduced unresolved in-flight publication race");}
'''
with tempfile.TemporaryDirectory(prefix='publisher-stop-') as d:
 p=Path(d)/'x.cpp';x=Path(d)/'x';p.write_text(pre+body+post)
 subprocess.run(['c++','-std=c++17','-pthread','-Wall','-Wextra','-Werror',str(p),'-o',str(x)],check=True)
 subprocess.run([str(x)],check=True)
