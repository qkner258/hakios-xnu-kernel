#!/usr/bin/env python3
"""Host-only CA augmentation. Run under env.sh/build-lock; never touches a phone.

Extracts the exact --base APFS into a temporary 40 MiB tree. Every original
file is compared after adding USB files; artifact copies use APFS clones.
"""
import argparse,hashlib,json,os,plistlib,shutil,subprocess,sys,tempfile
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--base',type=Path,required=True)
p.add_argument('--packages',type=Path,required=True);p.add_argument('--keys',type=Path,required=True)
p.add_argument('--out',type=Path,required=True)
a=p.parse_args();root=Path(__file__).resolve().parents[3];tool=root/'tools/apfs'
sys.path.insert(0,str(tool));import apfs
base=apfs.pick_volume(apfs.Container(str(a.base)),None)
baseline={}
for path,_,fid,dt in apfs.walk(base,apfs.ROOT_DIR_INO_NUM):
 ino=base.inode(fid); entry={'type':dt,'mode':ino.mode,'uid':ino.owner,'gid':ino.group}
 if dt in (8,10):entry['sha256']=hashlib.sha256(ino.contents()).hexdigest()
 baseline[path]=entry

def clone(src,dst):
 dst.parent.mkdir(parents=True,exist_ok=True);subprocess.run(['cp','-c',str(src),str(dst)],check=True)
a.out.mkdir(parents=True,exist_ok=True)
with tempfile.TemporaryDirectory(prefix='umi-usb-ca-') as temp:
 stage=Path(temp)/'root';stage.mkdir()
 for path,_,fid,dt in apfs.walk(base,apfs.ROOT_DIR_INO_NUM):
  ino=base.inode(fid);dest=stage/path.lstrip('/')
  if dt==4:dest.mkdir()
  elif dt==10:dest.symlink_to(ino.contents().decode())
  elif dt==8:dest.write_bytes(ino.contents())
  else:raise RuntimeError('unsupported inode '+path)
  if dt!=10:dest.chmod(ino.mode & 0o7777)
 for name in ('usb-network','usbmux'):
  subprocess.run([str(root/'xnu-deps/host/bin/xnu-clang'),'-arch','arm64','-isysroot',os.environ['SDK'],'-O1','-Wall','-Wextra','-o',str(stage/'bin'/name),str(Path(__file__).with_name(name+'.c'))],check=True)
 # Keep only executables used for SSH and the recursively referenced libraries.
 prefix=stage/'share/usb-jb'; originals=a.packages/'var/jb';pending=['usr/sbin/sshd','usr/sbin/pwd_mkdb','usr/libexec/sftp-server','usr/bin/dash'];seen=set();deps={}
 while pending:
  rel=pending.pop()
  if rel in seen:continue
  seen.add(rel);src=originals/rel;dst=prefix/rel;clone(src,dst)
  output=subprocess.check_output(['otool','-L',str(dst)],text=True)
  libs=[l.strip().split(' (',1)[0] for l in output.splitlines()[1:]];deps[rel]=libs
  for lib in libs:
   if lib.startswith('@rpath/'):pending.append('usr/lib/'+lib.split('/')[-1])
   elif lib.startswith('/var/jb/'):pending.append(lib[len('/var/jb/'):])
   else:assert lib.startswith(('/usr/lib/','/System/Library/')),lib
  # Procursus pwd_mkdb carries the macOS spelling of IOKit. iOS has no Versions/A.
  old='/System/Library/Frameworks/IOKit.framework/Versions/A/IOKit'
  if old in libs:
   subprocess.run(['install_name_tool','-change',old,'/System/Library/Frameworks/IOKit.framework/IOKit',str(dst)],check=True)
   subprocess.run(['codesign','-f','-s','-',str(dst)],check=True)
 clone(prefix/'usr/bin/dash',prefix/'bin/sh')
 # This is a minimal runtime, not an installed apt/Sileo bootstrap.
 for sub in ('var/empty','var/run','etc'): (prefix/sub).mkdir(parents=True,exist_ok=True)
 keys=stage/'share/usb-ssh';keys.mkdir(mode=0o700)
 for name in ('authorized_keys','ssh_host_ed25519_key','ssh_host_ed25519_key.pub'):
  clone(a.keys/name,keys/name);(keys/name).chmod(0o600)
 (keys/'sshd_config').write_text('''Port 22
ListenAddress 172.16.42.1
HostKey /redwood/share/usb-ssh/ssh_host_ed25519_key
AuthorizedKeysFile /redwood/share/usb-ssh/authorized_keys
PidFile /var/run/hakios-usb-sshd.pid
UsePAM no
PasswordAuthentication no
KbdInteractiveAuthentication no
PermitRootLogin prohibit-password
PubkeyAuthentication yes
AuthenticationMethods publickey
AllowUsers root
StrictModes yes
UseDNS no
PrintMotd no
Subsystem sftp /var/jb/usr/libexec/sftp-server
''')
 (stage/'LaunchDaemons/com.hakios.usb-network.plist').write_bytes(plistlib.dumps({
  'Label':'com.hakios.usb-network','ProgramArguments':['/redwood/bin/usb-network'],
  'RunAtLoad':True,'StandardOutPath':'/dev/console','StandardErrorPath':'/dev/console'}))
 # USB stage 3: device side of Apple's usbmux (/dev/hakios-usbmux) and Apple's own lockdownd job,
 # unchanged but for console stderr and os_log on the console (pairing/trust diagnostics).
 (stage/'LaunchDaemons/com.hakios.usbmux.plist').write_bytes(plistlib.dumps({
  'Label':'com.hakios.usbmux','ProgramArguments':['/redwood/bin/usbmux'],
  'RunAtLoad':True,'StandardOutPath':'/dev/console','StandardErrorPath':'/dev/console'}))
 lockdown=plistlib.loads((root/'sysimage/rootfs/System/Library/LaunchDaemons/com.apple.mobile.lockdown.plist').read_bytes())
 lockdown['StandardErrorPath']='/dev/console'
 lockdown.setdefault('EnvironmentVariables',{})['OS_ACTIVITY_DT_MODE']='YES'
 (stage/'LaunchDaemons/com.apple.mobile.lockdown.plist').write_bytes(plistlib.dumps(lockdown,fmt=plistlib.FMT_BINARY))
 out=a.out/'root-flash-ui-ca.apfs';assert not out.exists(), 'Refusing overwrite; clone backup first'
 subprocess.run([sys.executable,str(tool/'mkapfs.py'),'-z','--plain','fw','-o',str(out),'redwood='+str(stage)],check=True)
 new=apfs.pick_volume(apfs.Container(str(out)),None);changed=[];added=[]
 for path,_,fid,dt in apfs.walk(new,apfs.ROOT_DIR_INO_NUM):
  ino=new.inode(fid);entry={'type':dt,'mode':ino.mode,'uid':ino.owner,'gid':ino.group}
  if dt in (8,10):entry['sha256']=hashlib.sha256(ino.contents()).hexdigest()
  if path in baseline:
   if entry!=baseline.pop(path):changed.append(path)
  else:added.append(path)
 assert not baseline and not changed,(baseline.keys(),changed)
 (a.out/'ca-augmentation.json').write_text(json.dumps({'base':str(a.base),'base_sha256':hashlib.sha256(a.base.read_bytes()).hexdigest(),'original_files_changed':changed,'added':added,'ssh_dependencies':deps},indent=2)+'\n')
 clone(stage/'bin/usb-network',a.out/'usb-network'); clone(stage/'bin/usbmux',a.out/'usbmux')
 print('PASS: all original CA files unchanged; added',len(added),'USB/SSH entries')
