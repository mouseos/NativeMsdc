#!/usr/bin/env python3
"""Execute actual pre-worker hooks and fixed table; no target/device operations."""
from pathlib import Path
import os,subprocess,tempfile,re
R=Path(__file__).resolve().parents[1];s=(R/'NativeMsdc.cpp').read_text()
a=s.index('static void recordAttributedRead(');b=s.index('static void readAttributionThread(',a);hook=s[a:b]
a=s.index('static void startReadAttribution(');b=s.index('\n}\n\n\nextern "C"',a) if '\n}\n\n\nextern "C"' in s[a:] else s.index('\n}\n\nextern "C"',a)
lifecycle=s[a:b]
reporter=s[s.index('static void readAttributionThread('):s.index('static void startReadAttribution(')]
source=r'''
#include "NativeReadAttribution.h"
#include "NativeOverlapTrace.h"
#include <cassert>
#include <thread>
#include <vector>
#include <atomic>
#include <cstdio>
#include <cstdarg>
#include <string>
#include <type_traits>
constexpr NativeReadAttribution::Table constantTable{};
static_assert(sizeof(constantTable)==sizeof(NativeReadAttribution::Table),"constant initialization");
static_assert(std::is_trivially_destructible<NativeReadAttribution::Table>::value,"no teardown initializer");
static bool gReadAttributionEnabled=false,gReadAttributionStop=false,gReadAttributionStarted=false,gReadAttributionRequested=false;
static NativeReadAttribution::Table gReadAttribution;
static bool gOverlapTraceEnabled=false,gOverlapTraceRequested=false;
static NativeOverlapTrace::Buffer gOverlapTrace;
static NativeOverlapTrace::Snapshot gOverlapTraceSnapshot;
static uint64_t attributionNow();
static unsigned pidCalls=0,attrCalls=0,meta=0,passive=0,nocache=0;
static uint64_t gReadAttributionSkipped[4]={};
static void *current_thread(){return nullptr;}
static uint64_t thread_tid(void *){return 123;}
static int proc_selfpid(){pidCalls++;return 56;}
using bufattr_t=void *;
static int bufattr_meta(bufattr_t p){assert(p);attrCalls++;return meta;}
static int bufattr_passive(bufattr_t p){assert(p);attrCalls++;return passive;}
static int bufattr_nocache(bufattr_t p){assert(p);attrCalls++;return nocache;}
enum{kIODirectionIn=1,kIODirectionOut=2,kIOStorageOptionIsStatic=32};
struct IOMemoryDescriptor{unsigned direction=1;uint64_t length=16384;unsigned getDirection(){return direction;}uint64_t getLength(){return length;}};
struct IOStorageAttributes{uint16_t options=0;uint8_t priority=0,reserved0024=0;uint32_t reserved0032=0;uint64_t reserved0064=0,adjustedOffset=0;bufattr_t bufattr=nullptr;};
HOOK
struct IOService{bool inactive=false;bool isInactive(){return inactive;}unsigned refs=1;void retain(){refs++;}void release(){assert(refs);refs--;}};
static IOService *gReadAttributionOwner=nullptr;
static uint64_t gReadAttributionBegin;
using wait_result_t=int;using thread_t=void *;
static unsigned clockCalls=0,starts=0,deallocs=0,logs=0;static bool failThread=false;
static void (*pendingThread)(void *,wait_result_t);static void *pendingContext;
static void stopReadAttribution(IOService *);
static bool stopAtClock=false;
static uint64_t attributionNow(){clockCalls++;if(stopAtClock)stopReadAttribution(gReadAttributionOwner);return 123;}
static NativeReadAttribution::Snapshot gReadAttributionSnapshot;
static unsigned sleepCalls=0;static std::string output;
static void IOSleep(unsigned ms){assert(ms==10000);if(++sleepCalls==1){for(unsigned i=0;i<20;i++)gReadAttribution.record({i,1,0,0,i+1},4096,i);}else{gReadAttributionStop=true;gReadAttributionEnabled=false;}}
static int kernel_thread_start(void(*fn)(void*,wait_result_t),void *c,thread_t *out){starts++;if(failThread)return 1;pendingThread=fn;pendingContext=c;*out=c;return 0;}
static void thread_deallocate(thread_t){deallocs++;}
static void IOLog(const char *format,...) __attribute__((format(printf,1,2)));
static void IOLog(const char *format,...){logs++;char line[1024];va_list a;va_start(a,format);vsnprintf(line,sizeof line,format,a);va_end(a);output+=line;}
REPORTER
#define KERN_SUCCESS 0
LIFECYCLE
int main(){
IOMemoryDescriptor buffer;IOStorageAttributes attributes;
recordAttributedBlockRead(0,1,8,100,&buffer,&attributes);assert(!pidCalls&&!attrCalls);
gOverlapTraceEnabled=true;recordAttributedBlockRead(1,17,8,100,&buffer,&attributes,true);
NativeOverlapTrace::Snapshot trace;assert(gOverlapTrace.drain(trace));
assert(trace.count==1&&trace.events[0].pid==56&&trace.events[0].tid==123&&trace.events[0].window==1&&trace.events[0].block==17&&trace.events[0].count==8);
assert(!attrCalls);
for(unsigned i=0;i<32;i++)recordAttributedBlockRead(1,17,8,100,&buffer,&attributes,true);
unsigned fullPid=pidCalls,fullClock=clockCalls;
for(unsigned i=0;i<1000;i++)recordAttributedBlockRead(1,17,8,100,&buffer,&attributes,true);
assert(pidCalls==fullPid&&clockCalls==fullClock);
assert(gOverlapTrace.drain(trace)&&trace.count==32&&trace.full==1000);
recordAttributedBlockRead(1,17,8,100,&buffer,&attributes,false);assert(gOverlapTrace.drain(trace)&&trace.count==0);gOverlapTraceEnabled=false;pidCalls=0;clockCalls=0;

IOService owner,other;startReadAttribution(&owner,false);assert(!clockCalls&&!starts&&owner.refs==1);
startReadAttribution(&owner,true);assert(gReadAttributionEnabled&&clockCalls==1&&starts==1&&owner.refs==2&&deallocs==1&&pendingContext==&owner&&pendingThread);
startReadAttribution(&other,true);assert(starts==1&&other.refs==1);
meta=passive=nocache=1;attributes.options=kIOStorageOptionIsStatic;attributes.bufattr=&attributes;
recordAttributedBlockRead(0,1,8,100,&buffer,&attributes);
NativeReadAttribution::Snapshot snap;assert(gReadAttribution.drain(snap));assert(snap.calls==1&&snap.bytes==4096&&pidCalls==1&&attrCalls==3);
for(auto &row:snap.rows)if(row.used){assert(row.key.flags==31&&row.key.pid==56&&row.key.size==0&&row.firstBlock==1);}
recordAttributedBlockRead(0,1,0,100,&buffer,&attributes);recordAttributedBlockRead(0,100,1,100,&buffer,&attributes);
buffer.direction=kIODirectionOut;recordAttributedBlockRead(0,1,8,100,&buffer,&attributes);assert(pidCalls==1);
buffer.direction=3;recordAttributedBlockRead(0,1,8,100,&buffer,&attributes);assert(gReadAttributionSkipped[1]==2);
buffer.direction=kIODirectionIn;recordAttributedBlockRead(1,2,16,100,&buffer,nullptr);
recordAttributedRead(UINT32_MAX,32,4096,5);assert(pidCalls==3);assert(gReadAttribution.drain(snap)&&snap.calls==2);
stopReadAttribution(&other);assert(gReadAttributionEnabled);
logs=0;pendingThread(pendingContext,0);assert(owner.refs==1&&!gReadAttributionEnabled&&gReadAttributionOwner==nullptr);
assert(sleepCalls==2&&logs==30);assert(output.find("NativeMsdc attr stopped final_interval_unreported=1")!=std::string::npos);assert(output.find("other_calls=8 other_bytes=32768")!=std::string::npos);
output.clear();logs=0;
stopReadAttribution(&owner);assert(!gReadAttributionEnabled&&gReadAttributionStop);
// Fresh one-shot fixture: prove same-owner stop, without IOSleep setting the flag.
gReadAttributionStarted=false;gReadAttributionStop=false;failThread=false;
startReadAttribution(&owner,true);assert(owner.refs==2&&gReadAttributionEnabled);
const unsigned beforePid=pidCalls,beforeSleep=sleepCalls;
stopReadAttribution(&owner);assert(!gReadAttributionEnabled&&gReadAttributionStop);
recordAttributedRead(0,0,4096,0);assert(pidCalls==beforePid);
pendingThread(pendingContext,0);assert(owner.refs==1&&gReadAttributionOwner==nullptr&&sleepCalls==beforeSleep);
// Stop after owner publication but before enable/thread creation.
gReadAttributionStarted=false;gReadAttributionStop=false;stopAtClock=true;
unsigned beforeStarts=starts;startReadAttribution(&owner,true);
assert(starts==beforeStarts&&owner.refs==1&&!gReadAttributionEnabled&&gReadAttributionStop&&gReadAttributionOwner==nullptr);stopAtClock=false;
// Inactive service is refused before claiming Started or retaining.
gReadAttributionStarted=false;gReadAttributionStop=false;owner.inactive=true;
unsigned beforeClock=clockCalls;startReadAttribution(&owner,true);
assert(!gReadAttributionStarted&&starts==beforeStarts&&clockCalls==beforeClock&&owner.refs==1);owner.inactive=false;
gReadAttributionStarted=false;gReadAttributionOwner=nullptr;gReadAttributionStop=false;failThread=true;logs=0;startReadAttribution(&owner,true);assert(!gReadAttributionEnabled&&gReadAttributionStop&&owner.refs==1&&logs==1);
NativeReadAttribution::Table table;std::atomic<unsigned> done{0};std::vector<std::thread> threads;
for(unsigned j=0;j<4;j++)threads.emplace_back([&,j]{for(unsigned i=0;i<10000;i++)table.record({j,1,0,0,j+1},4096,i);done++;});
uint64_t total=0,bytes=0;
while(done<4){if(table.drain(snap)){uint64_t rows=0;for(auto &r:snap.rows)if(r.used)rows+=r.calls;assert(rows+snap.overflowCalls==snap.calls);total+=snap.calls+snap.contentionCalls;bytes+=snap.bytes+snap.contentionBytes;}}
for(auto &t:threads){t.join();}
assert(table.drain(snap));total+=snap.calls+snap.contentionCalls;bytes+=snap.bytes+snap.contentionBytes;
assert(total==40000&&bytes==40000ULL*4096);
// Trace-only reporter must not drain or emit attribution records.
gReadAttributionStarted=false;gReadAttributionOwner=nullptr;gReadAttributionStop=false;failThread=false;
assert(gReadAttribution.drain(snap));output.clear();logs=0;sleepCalls=0;
startReadAttribution(&owner,false,true);assert(!gReadAttributionEnabled&&gOverlapTraceEnabled);
recordAttributedBlockRead(1,17,8,100,&buffer,&attributes,true);
const unsigned clockBeforeReport=clockCalls;
pendingThread(pendingContext,0);
assert(output.find("NativeMsdc overlap event")!=std::string::npos);
assert(output.find("begin_ns=123 sample_ns=123")!=std::string::npos);
assert(output.find("NativeMsdc attr ")==std::string::npos);
assert(output.find("NativeMsdc overlap stopped final_interval_unreported=1")!=std::string::npos);
assert(clockCalls==clockBeforeReport+1);
assert(gReadAttribution.drain(snap)&&snap.calls==20);
// Both streams enabled: each gets its own framed report.
gReadAttributionStarted=false;gReadAttributionStop=false;sleepCalls=0;output.clear();logs=0;
startReadAttribution(&owner,true,true);recordAttributedBlockRead(1,17,8,100,&buffer,&attributes,true);
pendingThread(pendingContext,0);
assert(output.find("NativeMsdc overlap event")!=std::string::npos && output.find("prequeue=1")!=std::string::npos);
assert(output.find("NativeMsdc attr row")!=std::string::npos);


for(unsigned i=0;i<1000;i++)table.record({i,0,0,0,i+1},1,i);
assert(table.drain(snap)&&snap.calls==1000&&snap.overflowCalls>0);
}
'''.replace('HOOK',hook).replace('REPORTER',reporter).replace('LIFECYCLE',lifecycle)
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.cpp').write_text(source)
 subprocess.run([os.environ.get('CXX','c++'),'-DNATIVE_MSDC_BLOCK_DEVICES=1','-std=c++17','-pthread','-Wall','-Wextra','-Werror','-fsanitize='+os.environ.get('ATTR_SANITIZER','address,undefined'),'-I',str(R),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
 off=source[:source.index('int main(){')]+r"""
 int main(){IOService owner;startReadAttribution(&owner,false);stopReadAttribution(&owner);assert(!starts);gReadAttributionEnabled=true;recordAttributedRead(UINT32_MAX,32,4096,1);NativeReadAttribution::Snapshot snapshot;assert(gReadAttribution.drain(snapshot)&&snapshot.calls==1);}
 """
 # No BLOCK_DEVICES means attribute mock APIs become unused, as expected; only mocks are removed.
 a=off.index('using bufattr_t=');b=off.index('static void recordAttributedRead(',a)
 off=off[:a]+off[b:]
 off=off.replace('static unsigned pidCalls=0,attrCalls=0,meta=0,passive=0,nocache=0;','static unsigned pidCalls=0;')
 (p/'off.cpp').write_text(off)
 subprocess.run([os.environ.get('CXX','c++'),'-DNATIVE_MSDC_BLOCK_DEVICES=0','-std=c++17','-pthread','-Wall','-Wextra','-Werror','-fsanitize='+os.environ.get('ATTR_SANITIZER','address,undefined'),'-I',str(R),str(p/'off.cpp'),'-o',str(p/'off')],check=True)
 subprocess.run([str(p/'off')],check=True)
assert 'IOLog' not in hook and 'retain' not in hook and 'mach_absolute_time' not in hook
entry=s[s.index('IOReturn NativeMsdcBlock::doAsyncReadWrite'):s.index('IOReturn NativeMsdcBlock::readWriteNow')]
assert entry.index('recordAttributedBlockRead')<entry.index('if (!accepting) return kIOReturnNotReady')
assert entry.index('recordAttributedBlockRead')<entry.index('if (!completion || !completion->action) return kIOReturnBadArgument')
assert entry.index('recordAttributedBlockRead')<entry.index('BlockJob *job')
assert 'bootArgument("native-msdc-attribution", 1, 0) == 1' in s
assert 'bootArgument("native-msdc-attrib", 1, 0) == 1' in s
assert len('native-msdc-attrib=1')==len('native-msdc-iostat=1')==20
print('PASS actual pre-worker KPI hook, default-off, null/all attributes, extent/write refusal, store split, lifecycle failure/owner stop, fixed table contention/overflow accounting; ASan/UBSan')

start=s[s.index('bool NativeMsdc::start (IOService *provider)'):s.index('bool NativeMsdc::startRoot (')]
assert 'return false' not in start[start.index('startReadAttribution(this,'):]
print('PASS reporter body/top12/other/release and block-device-off helper compilation; reporter starts only after failure paths')

name_bound=int(re.search(r'char name\[(\d+)\]',(R/'NativeMsdc.h').read_text()).group(1))
for text in re.findall(r'IOLog\("(NativeMsdc (?:attr|overlap)[^"\n]*)"',s):
 line=bytes(text,'utf-8').decode('unicode_escape')
 line=re.sub(r'%llu','9'*20,line);line=re.sub(r'%x','f'*8,line);line=re.sub(r'%u','9'*10,line)
 line=line.replace('%.*s','X'*name_bound)
 assert '%' not in line and len(line)<=240,(len(line),line)
print('PASS complete diagnostic records <=240 bytes at full-width numeric values')
