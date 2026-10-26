#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Execute actual optional partition APIs against an instrumented host."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
source = (root / 'NativeMsdc.cpp').read_text()
api = source[source.index('extern "C" uint64_t native_msdc_part_size'):source.index('extern "C" int native_msdc_store_read')]
api = api[:api.rindex('#endif')]
code = r'''
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cassert>
#include <cstdio>
#include "NativeMsdc.h"
using namespace NativeMsdcHw;
struct {bool ready=true,layoutValid=true,poisoned=false;GptLayout gpt={};uint8_t bounce[65536];} gHost;
uint8_t gPartitionEntries[16384],disk[512*128];
unsigned locked=0,reads=0,fail=0;
struct HostSession {HostSession(){assert(!locked);locked++;}~HostSession(){locked--;}};
constexpr int kIOReturnSuccess=0;
static int transfer(uint64_t sector,unsigned blocks,uint8_t *out,bool write){
 assert(locked==1&&!write&&blocks==1&&sector>=40&&sector<=43);
 reads++;if(reads==fail)return 1;memcpy(out,disk+512*sector,512);return 0;
}
static void put64(uint8_t *p,uint64_t v){for(unsigned i=0;i<8;i++)p[i]=v>>(8*i);}
static void entry(unsigned n,const char *name,unsigned first,unsigned last){
 auto *p=gPartitionEntries+n*128;memset(p,0,128);p[0]=1;put64(p+32,first);put64(p+40,last);
 for(unsigned i=0;name[i];i++)p[56+2*i]=name[i];
}
static void checksum(){gHost.gpt.entriesCrc=gptCrc32(gPartitionEntries,gHost.gpt.entryCount*128);}
'''
code += api + r'''
int main(){
 gHost.gpt.entryCount=4;gHost.gpt.firstUsable=34;gHost.gpt.lastUsable=127;
 entry(0,"userdata",70,120);entry(1,"nvram",40,43);checksum();
 for(unsigned i=0;i<sizeof(disk);i++)disk[i]=uint8_t(i/512+i);
 assert(native_msdc_part_size("nvram")==2048);
 uint8_t output[1100];memset(output,0xcd,sizeof(output));
 assert(native_msdc_part_read("nvram",511,output+1,1025));
 assert(!memcmp(output+1,disk+40*512+511,1025)&&output[0]==0xcd&&output[1026]==0xcd);
 auto saved=reads;
 assert(!native_msdc_part_read("nvram",2047,output,2));
 assert(!native_msdc_part_read("nvram",UINT64_MAX,output,1));
 assert(!native_msdc_part_read("nvram",0,nullptr,1));
 assert(!native_msdc_part_read("nvram",0,output,0));
 assert(!native_msdc_part_read("missing",0,output,1));assert(reads==saved);
 for(unsigned n=1;n<=3;n++){reads=0;fail=n;assert(!native_msdc_part_read("nvram",511,output,1025));}
 fail=0;gHost.poisoned=true;assert(!native_msdc_part_size("nvram"));assert(!native_msdc_part_read("nvram",0,output,1));gHost.poisoned=false;
 entry(2,"nvram",45,47);checksum();assert(!native_msdc_part_size("nvram"));
 entry(2,"other",42,46);checksum();assert(!native_msdc_part_size("nvram"));
 memset(gPartitionEntries+256,0,128);checksum();gPartitionEntries[8]^=1;assert(!native_msdc_part_size("nvram"));gPartitionEntries[8]^=1;
 gHost.ready=false;assert(!native_msdc_part_size("nvram"));assert(!locked);
 puts("PASS actual named partition APIs: shared lock, read-only transfers, unaligned bounds, I/O failures, GPT CRC/duplicates/overlap, poisoned/unready refusal");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    p=Path(tmp); (p/'test.cpp').write_text(code)
    subprocess.run(['c++','-std=c++17','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I',str(root),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
