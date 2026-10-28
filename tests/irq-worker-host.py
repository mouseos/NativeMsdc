#!/usr/bin/env python3
"""Exercise the actual async queue, worker and synchronous barrier with host mocks."""
from pathlib import Path
import os,shlex,subprocess,tempfile
s=(Path(__file__).resolve().parents[1]/'NativeMsdc.cpp').read_text()
start=s.index('struct CompletionSlot;');end=s.index('\n#endif\n\nbool NativeMsdcBlock::configure',start)
queue=s[start:end];assert queue.endswith('\n}')
queue=queue[:-2] # outer namespace closing, since the mock runs in global namespace
start=s.index('IOReturn NativeMsdcBlock::doSynchronizeCache ()');end=s.index('\nIOReturn NativeMsdcBlock::synchronizeNow ()',start)
sync=s[start:end]+'\n#endif\n'
start=s.index('IOReturn NativeMsdcBlock::doAsyncReadWrite (');end=s.index('\nIOReturn NativeMsdcBlock::readWriteNow (',start)
asyncfn=s[start:end]+'\n#endif\n'
pre=r'''
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <unordered_map>
#include <memory>
#include <thread>
#include <vector>
using UInt64=uint64_t;using IOReturn=int;using wait_result_t=int;using thread_t=void*;
constexpr int kIOReturnSuccess=0,kIOReturnNotReady=1,kIOReturnNoMemory=2,kIOReturnBadArgument=3,kIOReturnAborted=4,kIOReturnError=5,THREAD_UNINT=0,KERN_SUCCESS=0;
#define NATIVE_MSDC_IRQ_WAIT 1
#define NATIVE_MSDC_WORKER_EXPERIMENT 1
#define NATIVE_MSDC_VERIFIED 1
constexpr int kIODirectionIn=1;
struct IOStorageAttributes{unsigned options;};struct IOStorageCompletion{void*target;void(*action)(void*,void*,int,uint64_t);void*parameter;};
struct IOMemoryDescriptor{int getDirection(){return kIODirectionIn;}uint8_t data[512]{};uint8_t expected=0;std::atomic<int> refs{1};void retain(){refs++;}void release(){refs--;}};
struct IOService{std::atomic<int> refs{1};void retain(){refs++;}void release(){refs--;}};
struct NativeMsdcBlock{bool verifiedActive=true,manifestReadOnly=true,writable=false;std::atomic<int> refs{1};void retain(){refs++;}void release(){refs--;}
 int doSynchronizeCache();int synchronizeNow();int readWriteNow(IOMemoryDescriptor*,UInt64,UInt64,IOStorageAttributes*,IOStorageCompletion*);
 int doAsyncReadWrite(IOMemoryDescriptor*,UInt64,UInt64,IOStorageAttributes*,IOStorageCompletion*);};
bool gActivationStopped=false;struct {bool irqReady=true;} gHost;thread_t gIoWorkerThread=nullptr;uint64_t native_msdc_worker_stats[4]{};
thread_local int ident;thread_t current_thread(){return &ident;}
using thread_call_param_t=void*;struct Call{void(*fn)(void*,void*);void*arg;};using thread_call_t=Call*;
constexpr int THREAD_CALL_PRIORITY_KERNEL=1;bool failCall=false;
thread_call_t thread_call_allocate_with_priority(void(*f)(void*,void*),void*a,int){return failCall?nullptr:new Call{f,a};}
bool thread_call_free(thread_call_t c){delete c;return true;}
void thread_call_enter(thread_call_t c){auto f=c->fn;auto a=c->arg;std::thread([=]{f(a,nullptr);}).detach();}
struct IOLock{std::mutex m;std::unordered_map<void*,std::shared_ptr<std::condition_variable>> events;};
IOLock* IOLockAlloc(){return new IOLock;}void IOLockFree(IOLock*p){delete p;}
void IOLockLock(IOLock*p){p->m.lock();}void IOLockUnlock(IOLock*p){p->m.unlock();}
int IOLockSleep(IOLock*p,void*event,int){auto &c=p->events[event];if(!c)c=std::make_shared<std::condition_variable>();std::unique_lock<std::mutex> l(p->m,std::adopt_lock);c->wait(l);l.release();return 0;}
void IOLockWakeup(IOLock*p,void*event,bool){auto it=p->events.find(event);if(it!=p->events.end())it->second->notify_all();}
std::atomic<int> startCount{0};int failStartAt=-1;std::atomic<bool> createEntered{false};bool blockCreate=false,createOpen=false;int blockAt=2;std::mutex createMutex;std::condition_variable createCv;
int kernel_thread_start(void(*fn)(void*,int),void *arg,thread_t *out){int n=startCount.fetch_add(1);if(n==failStartAt)return 1;if(blockCreate && n==blockAt){std::unique_lock<std::mutex> l(createMutex);createEntered=true;createCv.wait(l,[]{return createOpen;});}std::thread([=]{fn(arg,0);}).detach();*out=reinterpret_cast<void*>(1);return 0;}
void thread_deallocate(thread_t){}bool failAlloc=false;void* IOMalloc(size_t n){return failAlloc?nullptr:malloc(n);}void IOFree(void*p,size_t){free(p);}
'''
post=r'''
std::mutex callbackMutex;std::condition_variable callbackCv;std::atomic<int> callbacksHeld{0};std::atomic<bool> probeNested{false},nestedRejected{false};bool callbacksOpen=false;std::mutex records,gateMutex;std::condition_variable gateCv;std::atomic<bool> gateEntered{false};bool gateOpen=false,syncGate=false;
std::vector<uint64_t> order;std::atomic<int> completed{0},aborted{0};NativeMsdcBlock *reOwner=nullptr;IOMemoryDescriptor *reBuffer=nullptr;
void complete(void*,void*,int status,uint64_t bytes){if(status==kIOReturnAborted){assert(bytes==0);aborted++;}else{assert(status==0 && bytes==512);}completed++;}
void holdComplete(void*,void*,int status,uint64_t bytes){assert(status==0 && bytes==512);std::unique_lock<std::mutex> l(callbackMutex);int n=++callbacksHeld;l.unlock();
 if(n==1 && probeNested){while(callbacksHeld<64)std::this_thread::yield();IOStorageCompletion z{nullptr,complete,nullptr};nestedRejected=(reOwner->doAsyncReadWrite(reBuffer,3,1,nullptr,&z)==kIOReturnSuccess);}
 l.lock();callbackCv.wait(l,[]{return callbacksOpen;});completed++;}
void dataComplete(void* target,void*,int status,uint64_t bytes){auto *b=static_cast<IOMemoryDescriptor*>(target);assert(status==0 && bytes==512);for(auto x:b->data)assert(x==b->expected);completed++;}
void reenter(void*,void*,int status,uint64_t bytes){assert(status==0 && bytes==512);IOStorageCompletion c{nullptr,complete,nullptr};assert(reOwner->doAsyncReadWrite(reBuffer,2,1,nullptr,&c)==0);completed++;}
std::atomic<int> syncCycle{0};
void reenterWait(void*,void*,int status,uint64_t bytes){assert(status==0 && bytes==512);IOStorageCompletion c{nullptr,complete,nullptr};assert(reOwner->doAsyncReadWrite(reBuffer,2,1,nullptr,&c)==0);auto end=std::chrono::steady_clock::now()+std::chrono::milliseconds(100);while(completed==0 && std::chrono::steady_clock::now()<end)std::this_thread::yield();if(completed==0)syncCycle++;completed++;}
void stopComplete(void*,void*,int status,uint64_t bytes){assert(status==0 && bytes==512);stopBlockWorker();completed++;}
int NativeMsdcBlock::readWriteNow(IOMemoryDescriptor*buffer,UInt64 block,UInt64,IOStorageAttributes*,IOStorageCompletion*c){
 if(block==888){std::unique_lock<std::mutex> l(gateMutex);gateEntered=true;gateCv.wait(l,[]{return gateOpen;});}
 {std::lock_guard<std::mutex> l(records);order.push_back(block);}for(auto &x:buffer->data)x=uint8_t(block);c->action(c->target,c->parameter,0,512);return 0;}
int NativeMsdcBlock::synchronizeNow(){if(syncGate){std::unique_lock<std::mutex> l(gateMutex);gateEntered=true;gateCv.wait(l,[]{return gateOpen;});}return 0;}
void joinWorker(){for(;;){IOLockLock(gBlockQueueLock);bool done=!gBlockRunning;IOLockUnlock(gBlockQueueLock);if(done && gBlockParent->refs==1)break;std::this_thread::yield();}}
int main(int argc,char**argv){NativeMsdcBlock owner;IOMemoryDescriptor buffer;IOService parent;IOStorageCompletion c{nullptr,complete,nullptr};
 if(argc>1 && argv[1][0]=='p'){
  __atomic_store_n(&gActivationStopped,true,__ATOMIC_RELEASE);stopBlockWorker();assert(!prepareBlockWorker(&parent));
  assert(!startBlockWorker(&parent));assert(owner.doAsyncReadWrite(&buffer,1,1,nullptr,&c)==kIOReturnNotReady);return 0;}
 assert(prepareBlockWorker(&parent));
 if(argc>1 && argv[1][0]=='b'){
  blockCreate=true;blockAt=argv[1][1]-'0';std::atomic<bool> result{true};
  std::thread launching([&]{result=startBlockWorker(&parent);});while(!createEntered)std::this_thread::yield();
  __atomic_store_n(&gActivationStopped,true,__ATOMIC_RELEASE);stopBlockWorker();assert(owner.doAsyncReadWrite(&buffer,1,1,nullptr,&c)==kIOReturnNotReady);
  {std::lock_guard<std::mutex> l(createMutex);createOpen=true;}createCv.notify_all();launching.join();
  while(parent.refs!=1){std::this_thread::yield();} assert(!result && !gBlockPoolReady && completed==0);return 0;}
 if(argc>1 && argv[1][0]=='i'){
  failStartAt=argv[1][1]=='0'?0:1;assert(!startBlockWorker(&parent));assert(owner.doAsyncReadWrite(&buffer,1,1,nullptr,&c)==0);stopBlockWorker();assert(owner.doAsyncReadWrite(&buffer,1,1,nullptr,&c)==kIOReturnNotReady);while(parent.refs!=1)std::this_thread::yield();return 0;}
 assert(startBlockWorker(&parent));
 assert(!startBlockWorker(&parent));
 if(argc>1 && argv[1][0]=='k'){
  blockCreate=true;assert(owner.doAsyncReadWrite(&buffer,888,1,nullptr,&c)==0);while(!gateEntered)std::this_thread::yield();std::atomic<int> result{-1};
  std::thread submit([&]{result=owner.doAsyncReadWrite(&buffer,1,1,nullptr,&c);});while(!createEntered)std::this_thread::yield();stopBlockWorker();
  {std::lock_guard<std::mutex> l(createMutex);createOpen=true;}createCv.notify_all();submit.join();assert(result==kIOReturnNotReady);
  {std::lock_guard<std::mutex> l(gateMutex);gateOpen=true;}gateCv.notify_all();while(completed!=1)std::this_thread::yield();joinWorker();return 0;}
 if(argc>1 && argv[1][0]=='z'){
  failStartAt=2;assert(owner.doAsyncReadWrite(&buffer,888,1,nullptr,&c)==0);while(!gateEntered)std::this_thread::yield();
  assert(owner.doAsyncReadWrite(&buffer,1,1,nullptr,&c)==0 && completed==1);{std::lock_guard<std::mutex> l(gateMutex);gateOpen=true;}gateCv.notify_all();while(completed!=2)std::this_thread::yield();stopBlockWorker();joinWorker();return 0;}
 if(argc>1 && argv[1][0]=='a'){
  failAlloc=true;assert(owner.doAsyncReadWrite(&buffer,1,1,nullptr,&c)==0 && completed==1);failAlloc=false;stopBlockWorker();joinWorker();return 0;}
 if(argc>1 && argv[1][0]=='q'){
  assert(startCount==2);assert(owner.doAsyncReadWrite(&buffer,1,1,nullptr,&c)==0);while(completed!=1)std::this_thread::yield();stopBlockWorker();joinWorker();assert(startCount==2);return 0;}
 if(argc>1 && argv[1][0]=='h'){
  reOwner=&owner;reBuffer=&buffer;probeNested=true;IOStorageCompletion hold{nullptr,holdComplete,nullptr};for(int i=0;i<32;i++)assert(owner.doAsyncReadWrite(&buffer,i,1,nullptr,&hold)==0);
  while(callbacksHeld<32)std::this_thread::yield();
  for(int i=0;i<32;i++)assert(owner.doAsyncReadWrite(&buffer,i,1,nullptr,&hold)==0);
  while(callbacksHeld<64 || !nestedRejected){std::this_thread::yield();}assert(owner.doAsyncReadWrite(&buffer,1,1,nullptr,&c)==kIOReturnSuccess);
  assert(owner.doSynchronizeCache()==0);{std::lock_guard<std::mutex> l(callbackMutex);callbacksOpen=true;}callbackCv.notify_all();while(completed!=66)std::this_thread::yield();stopBlockWorker();joinWorker();return 0;}
 if(argc>1 && argv[1][0]=='y'){
  syncGate=true;std::thread in([&]{assert(owner.doSynchronizeCache()==0);});while(!gateEntered)std::this_thread::yield();stopBlockWorker();
  assert(owner.doSynchronizeCache()==kIOReturnNotReady);{std::lock_guard<std::mutex> l(gateMutex);gateOpen=true;}gateCv.notify_all();in.join();joinWorker();return 0;}
 if(argc>1 && argv[1][0]=='x'){
  owner.writable=true;std::thread in([&]{assert(owner.doAsyncReadWrite(&buffer,888,1,nullptr,&c)==0);});while(!gateEntered)std::this_thread::yield();
  stopBlockWorker();assert(owner.doAsyncReadWrite(&buffer,1,1,nullptr,&c)==kIOReturnNotReady);assert(owner.doSynchronizeCache()==kIOReturnNotReady);
  {std::lock_guard<std::mutex> l(gateMutex);gateOpen=true;}gateCv.notify_all();in.join();joinWorker();assert(completed==1 && owner.refs==1 && buffer.refs==1);return 0;}
 if(argc>1 && argv[1][0]=='d'){
  IOMemoryDescriptor data[24];for(int i=0;i<24;i++){data[i].expected=uint8_t(i);IOStorageCompletion z{&data[i],dataComplete,nullptr};assert(owner.doAsyncReadWrite(&data[i],i,1,nullptr,&z)==0);}
  while(completed!=24){std::this_thread::yield();}stopBlockWorker();joinWorker();for(auto &x:data)assert(x.refs==1);return 0;}
 if(argc>1 && argv[1][0]=='c'){
  reOwner=&owner;reBuffer=&buffer;IOStorageCompletion a{nullptr,reenterWait,nullptr};assert(owner.doAsyncReadWrite(&buffer,0,1,nullptr,&a)==0);while(completed!=2)std::this_thread::yield();stopBlockWorker();joinWorker();assert(syncCycle==0);return 0;}
 if(argc>1 && argv[1][0]=='r'){
  reOwner=&owner;reBuffer=&buffer;IOStorageCompletion a{nullptr,reenter,nullptr};assert(owner.doAsyncReadWrite(&buffer,888,1,nullptr,&a)==0);while(!gateEntered)std::this_thread::yield();
  assert(owner.doAsyncReadWrite(&buffer,1,1,nullptr,&c)==0);{std::lock_guard<std::mutex> l(gateMutex);gateOpen=true;}gateCv.notify_all();while(completed!=3)std::this_thread::yield();stopBlockWorker();joinWorker();assert(order.size()==3 && order[0]==888 && order[1]==1 && order[2]==2);return 0;}
 if(argc>1 && argv[1][0]=='s'){
  IOStorageCompletion a{nullptr,stopComplete,nullptr};assert(owner.doAsyncReadWrite(&buffer,888,1,nullptr,&a)==0);while(!gateEntered)std::this_thread::yield();
  assert(owner.doAsyncReadWrite(&buffer,1,1,nullptr,&c)==0);{std::lock_guard<std::mutex> l(gateMutex);gateOpen=true;}gateCv.notify_all();joinWorker();assert(completed==2 && ((aborted==1 && order.size()==1) || (aborted==0 && order.size()==2)));return 0;}
 if(argc>1 && argv[1][0]=='f'){
  assert(owner.doAsyncReadWrite(&buffer,888,1,nullptr,&c)==0);while(!gateEntered)std::this_thread::yield();for(int i=0;i<31;i++)assert(owner.doAsyncReadWrite(&buffer,i,1,nullptr,&c)==0);
  assert(owner.doAsyncReadWrite(&buffer,99,1,nullptr,&c)==kIOReturnSuccess);assert(owner.refs==33 && buffer.refs==33);
  assert(owner.doSynchronizeCache()==0);stopBlockWorker();{std::lock_guard<std::mutex> l(gateMutex);gateOpen=true;}gateCv.notify_all();joinWorker();assert(completed==33 && aborted==31);return 0;}
 for(int i=0;i<30;i++){assert(owner.doAsyncReadWrite(&buffer,i,1,nullptr,&c)==0);}while(completed!=30){std::this_thread::yield();}stopBlockWorker();joinWorker();
 assert(order.size()==30);for(int i=0;i<30;i++)assert(order[i]==(unsigned)i);assert(owner.refs==1 && buffer.refs==1 && parent.refs==1);assert(owner.doAsyncReadWrite(&buffer,0,1,nullptr,&c)==kIOReturnNotReady);return 0;}
'''
with tempfile.TemporaryDirectory(prefix='msdc-worker-') as d:
 p=Path(d)/'a.cpp';x=Path(d)/'a';p.write_text(pre+queue+sync+asyncfn+post)
 subprocess.run(['c++','-std=c++17','-O2','-Wall','-Wextra','-Werror','-pthread',*shlex.split(os.environ.get('MSDC_TEST_CXXFLAGS','')),str(p),'-o',str(x)],check=True)
 for mode in ('normal','reentry','stop','full','cycle','data','hardware','xstop','ysync','i0','i1','zfail','kstop','qsingle','allocfail','b0','b1','prestop'):
  subprocess.run([str(x),mode] if mode!='normal' else [str(x)],check=True,timeout=15)
 print('PASS FIFO/reentry, data snapshots, stop cancellation, full-queue sync and lifetimes')
 print('PASS nested read, returned media credits, saturation polling fallback, and direct I/O stop serialization')
