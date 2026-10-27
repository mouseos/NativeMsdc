#!/usr/bin/env python3
from pathlib import Path
import subprocess,tempfile
s=(Path(__file__).resolve().parents[1]/'NativeMsdc.cpp').read_text()
a=s.index('// Scan whole ASCII tokens:');b=s.index('\n}',s.index('bool verifiedCacheArgument ()',a))+2
h=r'''#include <cassert>
#include <cstring>
#include <cstddef>
static const char *args;
static const char *PE_boot_args(){return args;}
'''+s[a:b]+r'''
int main(){
 const char *yes[]={"mcv=1","native-msdc-vcache=1","mcv=0 native-msdc-vcache=1","mcv=0 mcv=1"};
 const char *no[]={"","mcv","mcv=0","mcv=2","mcv=01","mcv=4294967297","mcv=-4294967295","mcv=garbage","mcv=1 native-msdc-vcache=garbage","mcv=1 native-msdc-vcache","mcv=1 native-msdc-vcache=0","mcv=1 mcv=0","other=0\\nmcv=1","mcv=1\\rgarbage","xmc v=1"};
 for(auto v:yes){args=v;assert(verifiedCacheArgument());}
 for(auto v:no){args=v;assert(!verifiedCacheArgument());}
 args=nullptr;assert(!verifiedCacheArgument());
}
'''

with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.cpp').write_text(h)
 subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS verified cache argument defaults, exact values and long-name precedence')
