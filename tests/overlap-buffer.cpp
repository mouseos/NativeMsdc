#include "NativeOverlapTrace.h"
#include <cassert>
#include <thread>
#include <atomic>
#include <vector>
int main() {
  NativeOverlapTrace::Buffer b; NativeOverlapTrace::Snapshot s{};
  for (unsigned i=0;i<45;i++) b.record([&]{return NativeOverlapTrace::Event{i,10,i,1,1,1};});
  assert(b.drain(s) && s.count==32 && s.full==13 && s.contention==0);
  unsigned builds=0;
  for (unsigned i=0;i<32;i++) b.record([&]{++builds;return NativeOverlapTrace::Event{};});
  for (unsigned i=0;i<1000;i++) b.record([&]{++builds;return NativeOverlapTrace::Event{};});
  assert(builds==32 && b.drain(s) && s.count==32 && s.full==1000 && s.contention==0);

  std::atomic<bool> holding{false}, release{false};
  std::thread blocked([&]{b.record([&]{holding=true;while(!release.load()) std::this_thread::yield();return NativeOverlapTrace::Event{};});});
  while(!holding.load()) std::this_thread::yield();
  bool called=false;
  assert(!b.record([&]{called=true;return NativeOverlapTrace::Event{};}));
  assert(!called && !b.drain(s));
  release=true;blocked.join();
  assert(b.drain(s) && s.count==1 && s.contention==1 && s.full==0);
  std::atomic<unsigned> done{0}; std::vector<std::thread> threads;
  for(unsigned j=0;j<4;j++) threads.emplace_back([&,j]{for(unsigned i=0;i<10000;i++) b.record([&]{return NativeOverlapTrace::Event{i,j,i,1,j,1};});done++;});
  uint64_t total=0;
  while(done<4) if(b.drain(s)) total+=s.count+s.full+s.contention;
  for(auto &t:threads)t.join();
  assert(b.drain(s)); total+=s.count+s.full+s.contention;
  assert(total==40000);
}
