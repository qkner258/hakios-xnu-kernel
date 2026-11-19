#!/usr/bin/env python3
"""Host-only: exercise installd's signer/metadata with real Mach-O and Security."""
from pathlib import Path
import hashlib
import plistlib
import re
import shutil
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]


def run(*args):
    result = subprocess.run(list(map(str, args)), capture_output=True, text=True)
    if result.returncode:
        print(result.stdout, result.stderr)
    result.check_returncode()
    return result


def snapshot(path):
    return {str(p.relative_to(path)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in path.rglob('*') if p.is_file()}


def unsigned(path):
    data = path.read_bytes()
    assert struct.unpack_from('<I', data)[0] == 0xfeedfacf
    offset = 32
    for _ in range(struct.unpack_from('<I', data, 16)[0]):
        command, size = struct.unpack_from('<II', data, offset)
        assert command != 0x1d, 'fixture must have no LC_CODE_SIGNATURE'
        offset += size


def crypt(path):
    """Fake a FairPlay-encrypted App Store binary: set LC_ENCRYPTION_INFO_64's cryptid to 1."""
    data = bytearray(path.read_bytes())
    offset = 32
    for _ in range(struct.unpack_from('<I', data, 16)[0]):
        command, size = struct.unpack_from('<II', data, offset)
        if command == 0x2c:
            struct.pack_into('<I', data, offset + 16, 1)
            path.write_bytes(data)
            return
        offset += size
    raise AssertionError('fixture has no LC_ENCRYPTION_INFO_64')


def embedded_hashes(path):
    """Independently hash embedded CodeDirectories; require XML + DER slots."""
    data = path.read_bytes()
    offset = 32
    for _ in range(struct.unpack_from('<I', data, 16)[0]):
        command, size = struct.unpack_from('<II', data, offset)
        if command == 0x1d:
            start, length = struct.unpack_from('<II', data, offset + 8)
            signature = data[start:start + length]
            break
        offset += size
    else:
        raise AssertionError('missing LC_CODE_SIGNATURE after signing')
    magic, length, count = struct.unpack_from('>III', signature)
    assert magic == 0xfade0cc0 and length <= len(signature)
    slots = dict(struct.unpack_from('>II', signature, 12 + i * 8) for i in range(count))
    assert 5 in slots and 7 in slots, 'Runner needs XML and DER entitlements'
    hashes = []
    for slot, at in slots.items():
        if slot != 0 and not 0x1000 <= slot < 0x1005:
            continue
        magic, length = struct.unpack_from('>II', signature, at)
        assert magic == 0xfade0c02
        code_directory = signature[at:at + length]
        hash_type = code_directory[37]
        digest = hashlib.sha1 if hash_type == 1 else hashlib.sha256
        hashes.append(digest(code_directory).hexdigest()[:40])
    return hashes


HARNESS = r'''
#define HAKIOS_INSTALLD_HOST_TEST 1
#include "inject_installd.c"
#include <assert.h>
#include <stdlib.h>

static int fake_mis(CFStringRef path, CFDictionaryRef options, CFDictionaryRef *info, const void *progress)
{
    (void)path; (void)options; (void)progress;
    *info = NULL;
    return -402620388;
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    signer_key = CFSTR("SignerType"); cdhash_key = CFSTR("CdHash");
    entitlements_key = CFSTR("Entitlements"); identifier_key = CFSTR("SigningID");
    team_key = CFSTR("TeamID"); version_key = CFSTR("SignatureVersion");
    profile_key = CFSTR("ValidatedByProfile"); local_profile_key = CFSTR("ValidatedByLocalProfile");
    universal_profile_key = CFSTR("ValidatedByUniversalProfile");
    original_mis = fake_mis;
    CFStringRef path = CFStringCreateWithCString(NULL, argv[1], kCFStringEncodingUTF8);
    CFDictionaryRef info = NULL;
    int rc = hakios_mis(path, NULL, &info, NULL);
    if (atoi(argv[2]) == 0) {
        assert(rc == -402620388 && info == NULL);
        CFRelease(path);
        return 0;
    }
    assert(rc == 0 && info);
    CFURLRef url = CFURLCreateWithFileSystemPath(NULL, path, kCFURLPOSIXPathStyle, false);
    SecStaticCodeRef code = NULL;
    CFDictionaryRef disk = NULL;
    assert(SecStaticCodeCreateWithPath(url, 0, &code) == 0);
    assert(SecStaticCodeCheckValidity(code, 0, NULL) == 0);
    assert(SecCodeCopySigningInformation(code, 0x23, &disk) == 0);
    assert(CFEqual(CFDictionaryGetValue(info, cdhash_key), CFDictionaryGetValue(disk, kSecCodeInfoUnique)));
    assert(CFEqual(CFDictionaryGetValue(info, version_key), CFDictionaryGetValue(disk, kSecCodeInfoSignatureVersion)));
    assert(CFEqual(CFDictionaryGetValue(info, identifier_key), CFDictionaryGetValue(disk, kSecCodeInfoIdentifier)));
    CFDictionaryRef entitlements = CFDictionaryGetValue(disk, kSecCodeInfoEntitlementsDict);
    assert(entitlements && CFEqual(entitlements, CFDictionaryGetValue(info, entitlements_key)));
    assert(CFEqual(CFDictionaryGetValue(entitlements, CFSTR("application-identifier")),
                   CFDictionaryGetValue(info, identifier_key)));
    assert(!CFDictionaryContainsKey(info, team_key));
    assert(CFDictionaryGetValue(info, profile_key) == kCFBooleanFalse);
    CFRelease(disk); CFRelease(code); CFRelease(url); CFRelease(info); CFRelease(path);
    return 0;
}
'''


def main():
    # Keep all generated executables and fixtures in the build output tree.
    output = ROOT / 'out/xiaomi-mi-10'
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='installd-signing-', dir=output) as tmp:
        tmp = Path(tmp)
        (tmp / 'harness.c').write_text(HARNESS)
        run('xcrun', 'clang', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
            '-fsanitize=address,undefined',
            '-I', ROOT / 'userland/src', tmp / 'harness.c',
            '-framework', 'CoreFoundation', '-framework', 'Security', '-o', tmp / 'harness')
        (tmp / 'main.c').write_text('int main(void) { return 0; }\n')
        run('xcrun', 'clang', '-Wl,-no_adhoc_codesign', tmp / 'main.c', '-o', tmp / 'unsigned')
        unsigned(tmp / 'unsigned')

        def bundle(path, name, identifier, kind='APPL'):
            path.mkdir(parents=True)
            (path / 'Info.plist').write_bytes(plistlib.dumps(dict(
                CFBundleExecutable=name, CFBundleIdentifier=identifier, CFBundlePackageType=kind)))
            shutil.copy2(tmp / 'unsigned', path / name)
            return path

        app = bundle(tmp / 'Pili Plus.app', 'Runner', 'com.example.piliplus')
        signed = bundle(app / 'Frameworks/Signed.framework', 'Signed', 'com.example.Signed', 'FMWK')
        run('/usr/bin/codesign', '-s', '-', signed)
        original_signed = snapshot(signed)
        nested = bundle(app / 'Frameworks/Unsigned.framework', 'Unsigned', 'com.example.Unsigned', 'FMWK')
        extension = bundle(app / 'PlugIns/Share.appex', 'Share', 'com.example.piliplus.Share', 'XPC!')
        raw = app / 'Frameworks/libUnsigned.dylib'
        run('xcrun', 'clang', '-dynamiclib', '-Wl,-no_adhoc_codesign', tmp / 'main.c', '-o', raw)
        (app / 'resource.txt').write_text('resource, not code\n')
        (app / 'resource-link').symlink_to('resource.txt')
        result = run(tmp / 'harness', app, 1)
        assert 'unsigned -> ad-hoc' in result.stderr
        assert 'synthetic' not in result.stderr
        recorded = re.search(r'cdhash=([0-9a-f]{40})', result.stderr).group(1)
        assert recorded in embedded_hashes(app / 'Runner')
        for path in (app, nested, extension, raw):
            run('/usr/bin/codesign', '--verify', '--strict', path)
        assert snapshot(signed) == original_signed
        # Repeated validation must neither re-sign nor change the recorded hash.
        completed = snapshot(app)
        run(tmp / 'harness', app, 1)
        assert snapshot(app) == completed

        fat = bundle(tmp / 'Universal.app', 'Runner', 'com.example.universal')
        run('xcrun', 'clang', '-arch', 'arm64', '-arch', 'x86_64', '-Wl,-no_adhoc_codesign',
            tmp / 'main.c', '-o', fat / 'Runner')
        run(tmp / 'harness', fat, 1)
        run('/usr/bin/codesign', '--verify', '--all-architectures', '--strict', fat)

        ios = bundle(tmp / 'iOS.app', 'Runner', 'com.example.ios')
        sdk = ROOT.parents[1] / 'tools/theos/sdks/iPhoneOS16.5.sdk'
        run('xcrun', 'clang', '-target', 'arm64-apple-ios16.5', '-isysroot', sdk,
            '-Wl,-no_adhoc_codesign', tmp / 'main.c', '-o', ios / 'Runner')
        unsigned(ios / 'Runner')
        result = run(tmp / 'harness', ios, 1)
        recorded = re.search(r'cdhash=([0-9a-f]{40})', result.stderr).group(1)
        assert recorded in embedded_hashes(ios / 'Runner')
        run('/usr/bin/codesign', '--verify', '--strict', ios)

        # The walker must never follow a symlink into an unrelated executable.
        links = bundle(tmp / 'Links.app', 'Runner', 'com.example.links')
        external = tmp / 'outside'
        shutil.copy2(tmp / 'unsigned', external)
        (links / 'outside-link').symlink_to(external)
        run(tmp / 'harness', links, 1)
        unsigned(external)

        broken = bundle(tmp / 'Broken.app', 'Runner', 'com.example.broken')
        (broken / 'bad.dylib').write_bytes(struct.pack('<I', 0xfeedfacf))
        run(tmp / 'harness', broken, 0)
        unsigned(broken / 'Runner')
        run(tmp / 'harness', tmp / 'missing.app', 0)
        # A certificate signature libmis refused is replaced ad-hoc.
        shutil.copyfile('/usr/bin/true', tmp / 'apple')
        result = run(tmp / 'harness', tmp / 'apple', 1)
        assert 're-signed ad-hoc (was: certificate signature, status=0) 1 Mach-Os' in result.stderr
        run('/usr/bin/codesign', '--verify', '--all-architectures', '--strict', tmp / 'apple')

        # A signed app whose Info.plist and a nested resource seal were modified
        # afterwards (-67030 / -67061 class): every Mach-O is re-signed, entitlements kept.
        entitlements = tmp / 'keep.entitlements'
        entitlements.write_bytes(plistlib.dumps({'com.example.keep': True}))
        modified = bundle(tmp / 'Modified.app', 'Runner', 'com.example.modified')
        inner = bundle(modified / 'Frameworks/Inner.framework', 'Inner', 'com.example.Inner', 'FMWK')
        (inner / 'data.txt').write_text('sealed\n')
        share = bundle(modified / 'PlugIns/Share.appex', 'Share', 'com.example.modified.Share', 'XPC!')
        lib = modified / 'Frameworks/libSigned.dylib'
        run('xcrun', 'clang', '-dynamiclib', tmp / 'main.c', '-o', lib)
        watch = bundle(modified / 'Watch/Watch.app', 'Watch', 'com.example.modified.watch')
        for path in (inner, share, watch):
            run('/usr/bin/codesign', '-s', '-', path)
        run('/usr/bin/codesign', '-s', '-', '--entitlements', entitlements, modified)
        run('/usr/bin/codesign', '--verify', '--strict', '--deep', modified)
        (inner / 'data.txt').write_text('modified\n')
        info = plistlib.loads((modified / 'Info.plist').read_bytes())
        info['CFBundleShortVersionString'] = '2'
        (modified / 'Info.plist').write_bytes(plistlib.dumps(info))
        assert subprocess.run(['/usr/bin/codesign', '--verify', modified], capture_output=True).returncode
        before = snapshot(modified)
        result = run(tmp / 'harness', modified, 1)
        assert re.search(r're-signed ad-hoc \(was: ad-hoc signature, status=-670\d\d\) 5 Mach-Os', result.stderr)
        assert 're-signed -> ad-hoc' in result.stderr
        after = snapshot(modified)
        for name in ('Runner', 'Frameworks/Inner.framework/Inner', 'PlugIns/Share.appex/Share',
                     'Frameworks/libSigned.dylib', 'Watch/Watch.app/Watch'):
            assert before[name] != after[name], name
        run('/usr/bin/codesign', '--verify', '--strict', '--deep', modified)
        kept = plistlib.loads(run('/usr/bin/codesign', '-d', '--entitlements', '-', '--xml', modified).stdout.encode())
        assert kept == {'com.example.keep': True, 'application-identifier': 'com.example.modified'}, kept
        # The result is an intact ad-hoc signature: validating again changes nothing.
        run(tmp / 'harness', modified, 1)
        assert snapshot(modified) == after

        # FairPlay-encrypted main executable: refused, nothing signed.
        encrypted = bundle(tmp / 'Encrypted.app', 'Runner', 'com.example.encrypted')
        shutil.copy2(ios / 'Runner', encrypted / 'Runner')
        crypt(encrypted / 'Runner')
        before = snapshot(encrypted)
        result = run(tmp / 'harness', encrypted, 0)
        assert (f'hakios-installd: {encrypted} main executable is FairPlay-encrypted (cryptid=1); '
                'a decrypted IPA is needed') in result.stderr, result.stderr
        assert 'ad-hoc signed' not in result.stderr
        assert snapshot(encrypted) == before
    print('PASS: unsigned/nested/fat signing, embedded cdhash, XML/DER, metadata, seals, preservation, '
          'certificate/modified re-signing, FairPlay refusal, failure paths')


if __name__ == '__main__':
    main()
