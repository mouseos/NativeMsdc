#!/usr/bin/env python3
"""The only IRQ-wait callers must be reads on the owned publication thread."""
from pathlib import Path
s=(Path(__file__).resolve().parents[1]/'NativeMsdc.cpp').read_text()
assert 'bool publisherRead = false' in s
assert 'publisherRead &&\n      (self == __atomic_load_n (&gPublisherThread, __ATOMIC_ACQUIRE)' in s
assert 'self == __atomic_load_n (&gIoWorkerThread, __ATOMIC_ACQUIRE)' in s
assert 'ml_get_interrupts_enabled () && get_preemption_level () == 0' in s
assert '__atomic_store_n (&gPublisherThread, current_thread (), __ATOMIC_RELEASE)' in s
assert '__atomic_store_n (&gPublisherThread, static_cast<thread_t> (nullptr), __ATOMIC_RELEASE)' in s
approved=[line.strip() for line in s.splitlines() if 'transfer (' in line and ', false, true)' in line]
assert len(approved)==5,approved
assert any('kPolicyHeaderBytes' in line for line in approved)
assert any('region + done' in line for line in approved)
assert any('first + done' in line for line in approved)
assert 'transfer (firstSector + windowSector, blocks, out, false, true)' in s
assert 'transfer (firstSector + block + done, blocks, gHost.bounce, false, true)' in s
for name in ('native_msdc_store_open', 'native_msdc_store_read', 'native_msdc_store_write', 'native_msdc_store_sync', 'native_msdc_store_gpt_check'):
    start=s.index('extern "C"',s.index(name)-32)
    body=s[start:s.index('\n}',start)]
    assert 'OptionalBlockDirectGuard guard' in body, name
assert 'thread_call_allocate' not in s
print('PASS owner-thread IRQ wait and store lifecycle guards; no shared thread-call pool')
