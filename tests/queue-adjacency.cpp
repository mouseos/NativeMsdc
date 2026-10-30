#include "../NativeQueueAdjacency.h"
#include <cassert>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>
using namespace NativeQueueAdjacency;
int main() {
  Collector c; Snapshot s{};
  auto a=c.admit(1,100,8,1000000,0,true,nullptr);
  auto b=c.admit(1,108,8,2000000,1,true,&a);
  auto reverse=c.admit(1,92,8,3000000,2,true,&a);
  auto other=c.admit(2,108,8,4000000,3,true,&a);
  auto overlap=c.admit(1,104,8,5000000,4,true,&a);
  auto invalid=c.admit(1,108,8,6000000,5,false,&a);
  auto backward=c.admit(1,108,8,1,6,true,&a);
  assert(a.id==1 && b.predecessor==a.id && b.adjacent && reverse.adjacent && !other.adjacent && !overlap.adjacent && !invalid.valid && backward.clockBad);
  c.start(b,8000000);c.finish(b,0,4096,0,1,2);
  c.start(a,5000000);c.finish(a,1,0,0,1,2);
  c.start(invalid,7000000);c.finish(invalid,2,0,0,1,2);
  c.finish(other,3,0,0,1,2);
  c.drain(s);assert(s.admitted==7 && s.invalid==1 && s.tailPairs==5 && s.adjacent==3 && s.mergeable==3 && s.gap1ms==1 && s.gap5ms==2 && s.clockBad==1);
  assert(s.started==3 && s.waits[2]==1 && s.waits[1]==1 && s.waits[0]==1 && s.success==1 && s.authFailure==1 && s.aborted==1 && s.otherFailure==1 && s.partial==3 && s.eventCount==4 && !s.eventDropped);
  c.start(reverse,12000000); c.finish(reverse,0,4096,0,1,2);c.drain(s);assert(!s.admitted && s.started==1 && s.success==1 && s.events[0].request.id==reverse.id);
  auto longR=c.admit(0,0,128,1,0,true,nullptr);auto x=c.admit(0,128,1,2,1,true,&longR);assert(x.adjacent && !x.mergeable);
  auto max=c.admit(0,UINT64_MAX-1,8,2,1,true,&x);assert(!max.adjacent);
  for(int i=0;i<40;i++) c.finish(x,0,512,0,1,2);
  c.drain(s);assert(s.success==40 && s.eventCount==16 && s.eventDropped==24);
  Collector concurrent;std::mutex m;std::vector<std::thread> threads;
  for(int t=0;t<4;t++)threads.emplace_back([&]{for(int i=0;i<10000;i++){std::lock_guard<std::mutex> l(m);auto r=concurrent.admit(0,i,1,i,0,true,nullptr);concurrent.start(r,i+1);concurrent.finish(r,0,512,0,1,2);}});
  for(auto &t:threads) { t.join(); }
  concurrent.drain(s);assert(s.admitted==40000 && s.started==40000 && s.success==40000 && s.eventCount==16 && s.eventDropped==39984);
}
