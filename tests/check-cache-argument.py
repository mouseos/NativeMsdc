#!/usr/bin/env python3
from pathlib import Path
import subprocess,tempfile
s=(Path(__file__).resolve().parents[1]/'NativeMsdc.cpp').read_text()
a=s.index('bool verifiedCacheArgument ()');b=s.index('\n}',a)+2
h=r'''#include <cassert>
#include <cstring>
static int long_present, short_present; static unsigned long_value, short_value;
static bool PE_parse_boot_argn(const char *name,void *out,unsigned size) {
 assert(size==sizeof(unsigned));
 bool present = !strcmp(name,"native-msdc-vcache") ? long_present : (!strcmp(name,"mcv") ? short_present : false);
 if(present) *(unsigned*)out = !strcmp(name,"mcv") ? short_value : long_value;
 return present;
}
'''+s[a:b]+r'''
int main(){
 assert(!verifiedCacheArgument());short_present=1;short_value=1;assert(verifiedCacheArgument());
 short_value=0;assert(!verifiedCacheArgument());short_value=2;assert(!verifiedCacheArgument());
 short_value=1;long_present=1;long_value=0;assert(!verifiedCacheArgument());
 long_value=2;assert(!verifiedCacheArgument());long_value=1;short_value=0;assert(verifiedCacheArgument());
 long_present=0;short_present=0;assert(!verifiedCacheArgument());
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.cpp').write_text(h)
 subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS verified cache argument defaults, exact values and long-name precedence')
