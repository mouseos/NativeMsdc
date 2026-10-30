#!/usr/bin/env python3
"""Compile and exercise the real worker with wait-cause instrumentation enabled."""
from pathlib import Path
p = Path(__file__).with_name('irq-worker-host.py')
s = p.read_text()
header = (p.parent.parent / 'NativeQueueWaitCause.h').resolve()
s = s.replace('#define NATIVE_MSDC_VERIFIED 1', '#define NATIVE_MSDC_VERIFIED 1\n#define NATIVE_MSDC_WAIT_CAUSE 1\n#include "' + str(header) + '"\nint IOLog(const char*, ...){return 0;}\nvoid IOSleep(unsigned n){std::this_thread::sleep_for(std::chrono::milliseconds(n));}')
needle = 'void absolutetime_to_nanoseconds(uint64_t t,uint64_t*out){*out=t;}'
s = s.replace(needle, needle + '\nuint64_t attributionNow(){return mach_absolute_time();}')
s = s.replace('assert(startBlockWorker(&parent));', 'assert(startBlockWorker(&parent));\n __atomic_store_n(&gWaitCauseEnabled,true,__ATOMIC_RELEASE);')
assert 'NATIVE_MSDC_WAIT_CAUSE 1' in s and 'uint64_t attributionNow()' in s
exec(compile(s, str(p), 'exec'), {'__file__': str(p), '__name__': '__main__'})
