#!/usr/bin/env python3
"""Exercise the actual block-child start cancellation and inactive checks."""
from pathlib import Path
import subprocess,tempfile
s=(Path(__file__).resolve().parents[1]/'NativeMsdc.cpp').read_text()
a=s.index('bool NativeMsdcBlock::start (IOService *provider)');b=s.index('\nIOReturn NativeMsdcBlock::doSynchronizeCache',a)
body=s[a:b]
pre=r'''
#include <cassert>
#include <cstdio>
#include <atomic>
#include <thread>
bool gPublisherStopped=false, deactivateOnBase=false, cancelOnBase=false, baseOk=true; int registers=0, baseStops=0;
struct IOService{bool inactive=false;bool isInactive(){return inactive;}};
struct IOBlockStorageDevice{void stop(IOService*){++baseStops;} bool inactive=false; bool isInactive(){return inactive;} bool start(IOService*){if(deactivateOnBase)inactive=true;if(cancelOnBase)__atomic_store_n(&gPublisherStopped,true,__ATOMIC_RELEASE);return baseOk;}};
std::atomic<bool> entered{false},go{false};bool blockRegister=false;
struct NativeMsdcBlock:IOBlockStorageDevice {bool online=false;bool start(IOService*);void registerService(){entered=true;while(blockRegister && !go)std::this_thread::yield();++registers;}};
'''
post=r'''
int main(){IOService provider;NativeMsdcBlock a;gPublisherStopped=true;assert(!a.start(&provider)&&!a.online&&!registers);
 gPublisherStopped=false;provider.inactive=true;NativeMsdcBlock blockedParent;assert(!blockedParent.start(&provider)&&!registers);provider.inactive=false;
 NativeMsdcBlock b; b.inactive=true;assert(!b.start(&provider)&&!registers);
 NativeMsdcBlock c;deactivateOnBase=true;assert(!c.start(&provider)&&!c.online&&!registers&&baseStops==1);deactivateOnBase=false;
 NativeMsdcBlock d;cancelOnBase=true;assert(!d.start(&provider)&&!d.online&&!registers&&baseStops==2);cancelOnBase=false;gPublisherStopped=false;
 NativeMsdcBlock e;assert(e.start(&provider)&&e.online&&registers==1);
 NativeMsdcBlock f;blockRegister=true;entered=false;std::thread t([&]{assert(f.start(&provider));});
 while(!entered){std::this_thread::yield();}__atomic_store_n(&gPublisherStopped,true,__ATOMIC_RELEASE);go=true;t.join();
 assert(registers==2);puts("PASS actual checks; reproduced actual child start/register race after direct stop");}
'''
with tempfile.TemporaryDirectory(prefix='msdc-child-start-') as d:
 p=Path(d)/'test.cpp';x=Path(d)/'test';p.write_text(pre+body+post)
 subprocess.run(['c++','-std=c++17','-pthread','-Wall','-Wextra','-Werror',str(p),'-o',str(x)],check=True)
 subprocess.run([str(x)],check=True)
