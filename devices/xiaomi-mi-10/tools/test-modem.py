#!/usr/bin/env python3
"""Compile and run hardware-free tests of the actual UMI modem implementation."""
from pathlib import Path
import os
import subprocess
root=Path(__file__).resolve().parents[3]
device=root/'devices/xiaomi-mi-10'
out=root/'out/modem/tests'
out.mkdir(parents=True,exist_ok=True)
for name in ('pcie','mhi','stage3','bb'):
    binary=out/name
    subprocess.run([os.environ.get('CC','clang'),'-std=c11','-O1','-Wall','-Wextra','-Werror','-Wno-unused-function',
                    *(['-fsanitize=address,undefined'] if name=='bb' else []),
                    '-I'+str(device/'overlays/xnu/files/bsd/dev'),
                    str(device/'tools/modem-tests'/f'{name}.c'),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)

# Compile the actual loader gate so an accidental wildcard cannot admit the
# integrated modem's dangerous defaults or a near-match diagnostic token.
s=(device/'loader/loader.c').read_text()
start=s.index('static int\numi_bootargs_ok(')
end=s.index('\nstatic int\nchoose_window(',start)
shim='''#include <assert.h>
#include <stdint.h>
#include <string.h>
#define print(...) ((void)0)
static int platform_umi;
static int *PLAT=&platform_umi;
struct payload_info { uint64_t forced_base; const char *cmdline; };
static uint64_t str_len(const char *s) { return strlen(s); }
static int starts_with(const char *s,const char *p) {return strncmp(s,p,strlen(p))==0;}
'''
test='''
int main(void) {
 const char *good[]={"umi_modem=1", "umi_modem=2", "umi_modem_probe=1", "wifi=1 umi_modem_probe=1", "-v bootcpu=0 cpus=8 umi_touch=1 umi_keys=1 wifi=1 umi_modem_probe=1 loader_return_after=45"};
 const char *bad[]={"modem=1","modem_step=7","soc_mpss_mem=0x8b800000","umi_modem=3","umi_modem=22","umi_modem=11","umi_modem=1x","umi_modem_probe=11","umi_modem_probe="};
 struct payload_info p={0};
 for(unsigned i=0;i<sizeof(good)/sizeof(*good);i++){p.cmdline=good[i];assert(umi_bootargs_ok(&p));}
 for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++){p.cmdline=bad[i];assert(!umi_bootargs_ok(&p));}
 return 0;
}
'''
(out/'gate.c').write_text(shim+s[start:end]+test)
subprocess.run(['clang','-std=c11',str(out/'gate.c'),'-o',str(out/'gate')],check=True)
subprocess.run([str(out/'gate')],check=True)
print('PASS loader: exact external-modem arguments (umi_modem=1 kernel dial, 2 CommCenter bridge); integrated-MPSS and near-match tokens rejected')

import hashlib
import importlib.util
import tempfile
spec=importlib.util.spec_from_file_location('modem_stage',device/'tools/stage-modem-firmware.py')
stage=importlib.util.module_from_spec(spec)
spec.loader.exec_module(stage)
with tempfile.TemporaryDirectory(dir=out) as temporary:
    source=Path(temporary)
    (source/'sbl1.mbn').write_bytes(b'test-SBL-only')
    (source/'test.mbn').write_bytes(b'test-image-only')
    (source/'sahara.map').write_text('21 test.mbn\n')
    def checksum():
        (source/'SHA256SUMS').write_text(''.join(hashlib.sha256((source/n).read_bytes()).hexdigest()+'  '+n+'\n'
                                       for n in ('sbl1.mbn','test.mbn','sahara.map')))
    checksum()
    assert stage.validate(source)==['sahara.map','sbl1.mbn','test.mbn']
    (source/'test.mbn').write_bytes(b'corrupt')
    try:
        stage.validate(source)
    except ValueError:
        pass
    else:
        raise AssertionError('corrupt image accepted')
    (source/'sahara.map').write_text('21 ../test.mbn\n');checksum()
    try:
        stage.validate(source)
    except ValueError:
        pass
    else:
        raise AssertionError('path traversal accepted')
print('PASS firmware staging: required checksums, corruption and path-traversal rejection')
