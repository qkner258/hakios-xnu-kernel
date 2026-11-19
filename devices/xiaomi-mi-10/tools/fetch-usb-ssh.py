#!/usr/bin/env python3
"""Host-only: fetch checksum-verified Procursus files for the minimal USB SSH runtime.
No package maintainer scripts or target executables are run.
"""
from pathlib import Path
import sys,subprocess,hashlib,json,tarfile,io,argparse
repo=Path(__file__).resolve().parents[3]
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--out',required=True,type=Path);a=p.parse_args()
scratch=a.out.resolve();scratch.mkdir(parents=True,exist_ok=True)
if not (scratch/'Packages').is_file(): subprocess.run(['curl','-sSL','--fail','--max-time','60','https://apt.procurs.us/dists/iphoneos-arm64-rootless/1800/main/binary-iphoneos-arm64/Packages','-o',str(scratch/'Packages')],check=True)
sys.path.insert(0,str(repo/'tools'));import importlib.util
spec=importlib.util.spec_from_file_location('jb',repo/'tools/jb-preinstall.py');jb=importlib.util.module_from_spec(spec);spec.loader.exec_module(jb)
names='openssh-server openssh-sftp-server libssl3 libiosexec1 libcrypt2 libpam2 libmd0 system-cmds file-cmds dash libedit0 libncursesw6'.split()
best={}
for s in jb.stanzas((scratch/'Packages').read_text()):
 n=jb.field(s,'Package')
 if n in names and (n not in best or jb.newer(jb.field(s,'Version'),jb.field(best[n],'Version'))): best[n]=s
out=scratch/'extract';out.mkdir(exist_ok=True)
manifest={}
for n in names:
 s=best[n]; url='https://apt.procurs.us/'+jb.field(s,'Filename'); dest=scratch/(n+'.deb')
 if not dest.exists(): subprocess.run(['curl','-sSL','--fail','--max-time','60',url,'-o',str(dest)],check=True)
 digest=hashlib.sha256(dest.read_bytes()).hexdigest();assert digest==jb.field(s,'SHA256'),n
 manifest[n]={'version':jb.field(s,'Version'),'url':url,'sha256':digest}
 members=subprocess.check_output(['ar','t',str(dest)],text=True).splitlines();m=next(m for m in members if m.startswith('data.tar'))
 data=subprocess.check_output(['ar','p',str(dest),m])
 if m.endswith('.zst'):data=subprocess.check_output(['zstd','-dc'],input=data)
 with tarfile.open(fileobj=io.BytesIO(data)) as tar:
  for member in tar:
   path=Path(member.name)
   assert not path.is_absolute() and '..' not in path.parts
   if not str(path).startswith('var/jb/'):continue
   # Package scripts are not run. Files stay inside this scratch tree.
   for parent in path.parents:
    assert not (out/parent).is_symlink(),str(path)
   tar.extract(member,out,filter='fully_trusted')
 print('verified',n,manifest[n]['version'])
(scratch/'packages.json').write_text(json.dumps(manifest,indent=2)+'\n')
