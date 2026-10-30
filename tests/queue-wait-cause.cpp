#include "../NativeQueueWaitCause.h"
#include <cassert>
#include <cstdio>
using namespace NativeQueueWaitCause;
int main() {
  Collector c; Snapshot s{};
  c.dequeue(10, 10000010, 1); // exactly 10 ms excluded
  c.dequeue(10, 10000011, 1); // no predecessor
  c.finished(8, 100, 1000100, true); // 4 KiB, 1 ms
  c.dequeue(50, 10000051, 1); // arrived before predecessor
  c.dequeue(1000100, 11000101, 2); // at its finish, in predecessor
  c.dequeue(1000101, 11000102, 8); // arrived after predecessor
  c.finished(128, 100, 5000100, true); // 64 KiB, 5 ms
  c.dequeue(99, 10000100, 9);
  c.finished(129, 100, 10000100, true); // >64 KiB, 10 ms
  c.dequeue(99, 10000101, 3);
  c.finished(4096, 100, 10000101, true); // >10 ms
  c.dequeue(100, 20000101, 2);
  c.dequeue(200, 100, 1); // clock regression
  c.dequeue(0, 10000001, 0); // invalid depth
  c.drain(s);
  assert(s.longWait == 7 && s.unknownPrevious == 1 && s.badInput == 2 && s.badPrevious == 0 && !s.overflow);
  assert(s.relation[0] == 3 && s.relation[1] == 2 && s.relation[2] == 1);
  assert(s.cells[0][0][0][0] == 1 && s.cells[0][0][1][1] == 1 && s.cells[0][0][2][2] == 1);
  assert(s.cells[1][1][0][3] == 1 && s.cells[2][2][0][2] == 1 && s.cells[2][3][1][1] == 1);
  c.dequeue(100, 20000101, 1); c.drain(s); // predecessor survives drain
  assert(s.longWait == 1 && s.cells[2][3][1][0] == 1);
  c.finished(0, 1, 2, true); c.dequeue(1, 10000002, 1); c.drain(s);
  assert(s.longWait == 1 && s.unknownPrevious == 1);
  c.finished(8, 3, 2, true); c.dequeue(1, 10000002, 1); c.drain(s);
  assert(s.longWait == 1 && s.unknownPrevious == 1);
  c.finished(8, 100, 20000000, true); c.dequeue(1, 10000002, 1); c.drain(s);
  assert(s.longWait == 1 && s.badPrevious == 1);
  // Exercise a whole distribution well beyond a 16-event example buffer.
  c.finished(4096, 100, 10000101, true);
  for (unsigned i = 0; i < 50000; ++i)
    c.dequeue(i % 2 ? 100 : 99, 20000101, i % 2 ? 2 : 9);
  c.drain(s);
  assert(s.longWait == 50000 && s.relation[0] == 25000 && s.relation[1] == 25000);
  assert(s.cells[2][3][0][3] == 25000 && s.cells[2][3][1][1] == 25000);
  uint64_t counter = UINT64_MAX - 1, overflow = 0;
  incrementSaturating(counter, overflow); assert(counter == UINT64_MAX && !overflow);
  incrementSaturating(counter, overflow); assert(counter == UINT64_MAX && overflow == 1);
  std::puts("PASS wait cause boundaries, relation, prior and drain");
}
