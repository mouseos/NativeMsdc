#!/usr/bin/env python3
"""Compile actual invalidation/configuration entry paths with IOKit mocks."""
from pathlib import Path
import subprocess,tempfile
R=Path(__file__).resolve().parents[1]
s=(R/'NativeMsdc.cpp').read_text()
def fun(sig):
 a=s.index(sig);e=s.index('{',a)+1;d=1
 while d:d+=(s[e]=='{')-(s[e]=='}');e+=1
 return s[a:e]
configure=fun('bool NativeMsdcBlock::configure (')
a=s.index('class VerifiedWindowGuard');b=s.index('\n};',a)+3;guard=s[a:b]
a=s.index('const char *NativeMsdcBlock::configureVerification');a=s.index('{',a)+1;b=s.index('  if (!gPolicy.storage',a);failed=s[a:b]
a=s.index('IOReturn NativeMsdcBlock::doAsyncReadWrite');a=s.index('{',a)+1;b=s.index('  if (!buffer ||',a);write=s[a:b]
head=r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#define NATIVE_MSDC_VERIFIED 1
#include "NativeMsdc.h"
using namespace NativeMsdcHw;
struct IOLock{bool held=false;};
static IOLock*IOLockAlloc(){return new IOLock;}
static void IOLockLock(IOLock*l){assert(l&&!l->held);l->held=true;}
static void IOLockUnlock(IOLock*l){assert(l&&l->held);l->held=false;}
#define strlcpy fixture_strlcpy
static size_t fixture_strlcpy(char*d,const char*s,size_t n){size_t z=strlen(s);if(n){size_t k=z<n-1?z:n-1;memcpy(d,s,k);d[k]=0;}return z;}
static struct{struct{uint64_t userdataFirst=100,userdataLast=10000;}gpt;}gHost;
static constexpr int kIODirectionOut=2;
struct Buffer{int direction;int getDirection(){return direction;}};
'''
# RootWindow/kWindowReadOnly/kSector come from the shipped header.
klass=r'''
class NativeMsdcBlock{public:
IOLock*verifiedLock=nullptr;VerifiedReadCache verifiedCache;uint64_t verifiedConfiguration=0;
bool verifiedActive=false,online=true,manifestReadOnly=false,writable=false,verifiedCacheEnabled=false;
struct {uint64_t generation=1;} verified;unsigned window=0;
uint64_t firstSector=0,sectors=0;char label[32]={};
bool configure(unsigned,const RootWindow&,uint64_t,bool);
const char*failedVerification(int*status);
void attemptedWrite(Buffer*buffer);
};
'''
main=r'''
int main(){NativeMsdcBlock x;RootWindow entry={};entry.size=4096;strcpy(entry.name,"OS");entry.flags=kWindowReadOnly;
assert(x.configure(1,entry,100,true));assert(x.manifestReadOnly&&!x.writable&&x.verifiedConfiguration==1&&!x.verifiedLock);
x.verifiedLock=IOLockAlloc();
x.verifiedCache.valid=true;x.verifiedActive=true;entry.size=0;
assert(!x.configure(1,entry,100,true)&&!x.verifiedCache.valid&&!x.verifiedActive&&!x.online&&!x.verifiedLock->held);
x.verifiedCache.valid=true;x.verifiedActive=true;x.online=true;int status=0;
assert(x.failedVerification(&status)&&status==-1&&!x.verifiedCache.valid&&!x.verifiedActive&&!x.online&&!x.verifiedLock->held);
Buffer out={kIODirectionOut},in={1};
x.verifiedCache.valid=true;x.verifiedCacheEnabled=false;x.attemptedWrite(&out);assert(x.verifiedCache.valid);
x.verifiedCacheEnabled=true;x.manifestReadOnly=false;x.attemptedWrite(&out);assert(x.verifiedCache.valid);
x.manifestReadOnly=true;x.verifiedCache.valid=true;x.attemptedWrite(&out);assert(!x.verifiedCache.valid&&!x.verifiedLock->held);
x.verifiedCache.valid=true;x.attemptedWrite(&in);assert(x.verifiedCache.valid);
x.attemptedWrite(nullptr);assert(x.verifiedCache.valid);
delete x.verifiedLock;puts("NativeMsdc actual configure/failed-verification/refused-write invalidation passed");}
'''
a=s.index('IOReturn NativeMsdcBlock::readVerified');a=s.index('    void lock ()',a)
end=s.index('    void unlock ()',a)
read_lock=s[a:end]
readops = 'struct ReadOps { IOLock *mutex; NativeMsdcBlock *owner; VerifiedReadKey *key; uint64_t firstSector=0,expectedConfiguration=0,expectedWindowBytes=4096;'+read_lock+r'''
void unlock(){IOLockUnlock(mutex);} int ioError(){return 2;} int badArgument(){return 1;}
int read(uint64_t,uint32_t,uint8_t*){abort();} int verify(uint64_t,const uint8_t*,size_t){abort();}
uint64_t copy(uint64_t,const uint8_t*,uint64_t){abort();}
};
'''
main=main.replace('delete x.verifiedLock;',r'''
VerifiedReadKey key={};uint8_t bounce[65536]={};x.verifiedActive=true;x.online=true;x.sectors=8;
ReadOps ops={x.verifiedLock,&x,&key};ops.expectedConfiguration=x.verifiedConfiguration-1;
auto stale=verifiedReadWindow(ops,4096,0,1,bounce,sizeof(bounce),&x.verifiedCache,&key);
assert(stale.status==2&&!stale.done&&!x.verifiedLock->held);
ops.expectedConfiguration=x.verifiedConfiguration;ops.expectedWindowBytes=8192;
stale=verifiedReadWindow(ops,4096,0,1,bounce,sizeof(bounce),&x.verifiedCache,&key);
assert(stale.status==2&&!stale.done&&!x.verifiedLock->held);
// Matching geometry still rejects an offline/reconfigured window before I/O.
ops.expectedWindowBytes=4096;x.online=false;
stale=verifiedReadWindow(ops,4096,0,1,bounce,sizeof(bounce),&x.verifiedCache,&key);
assert(stale.status==2&&!stale.done&&!x.verifiedLock->held);
delete x.verifiedLock;
''')
source=head+guard+klass+readops+configure+'\nconst char*NativeMsdcBlock::failedVerification(int*status){'+failed+'return "policy rejected";}\nvoid NativeMsdcBlock::attemptedWrite(Buffer*buffer){'+write+'(void)configuration;}\n'+main
with tempfile.TemporaryDirectory() as t:
 p=Path(t);(p/'t.cpp').write_text(source)
 subprocess.run(['c++','-std=c++17','-O1','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-pie','-no-pie','-I',str(R),str(p/'t.cpp'),'-o',str(p/'t')],check=True)
 subprocess.run([str(p/'t')],check=True)
