#!/usr/bin/env python3
"""The only IRQ-wait callers must be reads on the owned publication thread."""
from pathlib import Path
s=(Path(__file__).resolve().parents[1]/'NativeMsdc.cpp').read_text()
assert 'bool publisherRead = false' in s
assert 'publisherRead &&\n      __atomic_load_n (&gPublisherThread, __ATOMIC_ACQUIRE) == current_thread ()' in s
assert 'ml_get_interrupts_enabled () && get_preemption_level () == 0' in s
assert '__atomic_store_n (&gPublisherThread, current_thread (), __ATOMIC_RELEASE)' in s
assert '__atomic_store_n (&gPublisherThread, static_cast<thread_t> (nullptr), __ATOMIC_RELEASE)' in s
approved=[line.strip() for line in s.splitlines() if 'transfer (' in line and ', false, true)' in line]
assert len(approved)==3,approved
assert any('kPolicyHeaderBytes' in line for line in approved)
assert any('region + done' in line for line in approved)
assert any('first + done' in line for line in approved)
assert 'transfer (firstSector + windowSector, blocks, out, false)' in s
assert 'transfer (firstSector + block + done, blocks, gHost.bounce, false)' in s
print('PASS explicit publisher-only IRQ wait; block and store callers retain poll default')
