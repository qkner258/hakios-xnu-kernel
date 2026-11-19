#!/usr/bin/env python3
"""Host-only: compile the plain-C part of the Sandbox stand-in (kern_minimac.c, between the
host-testable markers) and check the seatbelt-profiles reader, the table of sandboxed processes,
the extension token, the application path test, the log rate and the layouts of the request blocks (libsystem_sandbox 1846.120.15, 20F66).

Optional: MINISANDBOX_BINARY=path/to/MessagesBlastDoorService also reads the profile from the XML
entitlements of a real signed binary (expects blastdoor-messages)."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'overlays/xnu/files/bsd/kern/kern_minimac.c').read_text()
begin, end = '/* host-testable: begin */', '/* host-testable: end */'
part = source[source.index(begin) + len(begin):source.index(end)]

real_xml = ''
binary = os.environ.get('MINISANDBOX_BINARY')
if binary:
    data = Path(binary).read_bytes()
    start = data.index(b'<?xml')
    real_xml = data[start:data.index(b'</plist>', start) + len('</plist>')].decode()

program = r'''
#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
''' + part + r'''
static int profile_of(const char *xml, char *name, size_t size)
{
    return minisandbox_seatbelt_profile(xml, strlen(xml), name, size);
}

int main(void)
{
    char name[MINISANDBOX_PROFILE_MAX], small[8], token[MINISANDBOX_TOKEN_SIZE];

    /* the blocks, as _sandbox_check_common, _sandbox_enable_state_flag and __sandbox_extension_issue fill them */
    assert(offsetof(struct minisandbox_check_args, pid) == 0x08);
    assert(offsetof(struct minisandbox_check_args, operation) == 0x10);
    assert(offsetof(struct minisandbox_check_args, flags) == 0x28);
    assert(offsetof(struct minisandbox_check_args, id) == 0x30);
    assert(sizeof(struct minisandbox_check_args) == 0x38);     /* the caller's block is 0x68 */
    assert(offsetof(struct minisandbox_state_flag_args, name) == 0x10);
    assert(offsetof(struct minisandbox_issue_args, token) == 0x20);
    assert(offsetof(struct minisandbox_issue_args, pid) == 0x28);
    assert(offsetof(struct minisandbox_issue_args, pidversion) == 0x30);

    /* seatbelt-profiles, laid out as in MessagesBlastDoorService */
    const char *blastdoor =
        "<?xml version=\"1.0\"?>\n<plist version=\"1.0\">\n<dict>\n"
        "\t<key>com.apple.private.security.enable-state-flags</key>\n\t<array>\n\t\t<string>blastdoor-post-launch</string>\n\t</array>\n"
        "\t<key>seatbelt-profiles</key>\n\t<array>\n\t\t<string>blastdoor-messages</string>\n\t</array>\n</dict>\n</plist>\n";
    assert(profile_of(blastdoor, name, sizeof(name)) == 1 && strcmp(name, "blastdoor-messages") == 0);
    assert(profile_of("<dict><key>seatbelt-profiles</key><array><string>a</string><string>b</string></array></dict>",
        name, sizeof(name)) == 1 && strcmp(name, "a") == 0);
    assert(profile_of("<dict><key>platform-application</key><true/></dict>", name, sizeof(name)) == 0);
    assert(profile_of("<dict><key>seatbelt-profiles</key><array/></dict>", name, sizeof(name)) == 0);
    assert(profile_of("<dict><key>seatbelt-profiles</key>\n<array>\n</array></dict>", name, sizeof(name)) == 0);
    assert(profile_of("<dict><key>seatbelt-profiles</key><array><string></string></array>", name, sizeof(name)) == 0);
    assert(profile_of("<dict><key>seatbelt-profiles</key><array><true/></array>", name, sizeof(name)) == 0);
    assert(profile_of("<dict><key>seatbelt-profiles</key><string>x</string>", name, sizeof(name)) == 0);
    assert(profile_of("<dict><key>com.example.seatbelt-profiles</key><array><string>x</string></array>", name, sizeof(name)) == 0);
    assert(profile_of("<dict><key>seatbelt-profiles</key><array><string>cut-off", name, sizeof(name)) == 0);
    assert(profile_of("<dict><key>seatbelt-prof", name, sizeof(name)) == 0);
    assert(profile_of("", name, sizeof(name)) == 0);
    /* the length is what counts, not a terminator: the blob in a signature has none */
    {
        const char *text = "<key>seatbelt-profiles</key><array><string>mobilesafari</string></array>";
        assert(minisandbox_seatbelt_profile(text, strlen(text) - 20, name, sizeof(name)) == 0);
        assert(minisandbox_seatbelt_profile(text, strlen(text), small, sizeof(small)) == 1 && strcmp(small, "mobiles") == 0);
    }
    if (REAL_XML[0]) {
        assert(profile_of(REAL_XML, name, sizeof(name)) == 1 && strcmp(name, "blastdoor-messages") == 0);
        printf("real binary: seatbelt-profiles -> %s\n", name);
    }

    /* the table */
    assert(minisandbox_lookup(7) == NULL);
    assert(minisandbox_record(7, "blastdoor-messages") == 0);
    assert(minisandbox_record(7, "other") == EEXIST && strcmp(minisandbox_lookup(7)->profile, "blastdoor-messages") == 0);
    assert(minisandbox_record(8, "a-profile-name-much-longer-than-the-forty-bytes-kept") == 0);
    assert(strlen(minisandbox_lookup(8)->profile) == MINISANDBOX_PROFILE_MAX - 1);
    minisandbox_lookup(8)->flags |= MINISANDBOX_REPORTED;
    minisandbox_forget(7);
    assert(minisandbox_lookup(7) == NULL && minisandbox_lookup(8) != NULL && minisandbox_lookup(8)->flags == MINISANDBOX_REPORTED);
    minisandbox_forget(7);
    minisandbox_forget(8);
    assert(minisandbox_nprocs == 0);
    for (uint64_t id = 1; id <= MINISANDBOX_MAX_PROCS; id++) {
        assert(minisandbox_record(id, "p") == 0);
    }
    assert(minisandbox_record(5000, "p") == ENOSPC);
    minisandbox_forget(MINISANDBOX_MAX_PROCS);          /* the last one */
    minisandbox_forget(1);                              /* the first one: the last moves into its place */
    assert(minisandbox_lookup(1) == NULL && minisandbox_lookup(2) != NULL && minisandbox_lookup(MINISANDBOX_MAX_PROCS - 1) != NULL);
    assert(minisandbox_record(5000, "p") == 0 && minisandbox_nprocs == MINISANDBOX_MAX_PROCS - 1);

    /* the token */
    assert(minisandbox_token(token, sizeof(token), "com.apple.app-sandbox.read", "/var/mobile/Media/x.jpg") == 0);
    assert(strcmp(token, "0000000000000000000000000000000000000000000000000000000000000000;00;00000000;00000000;00000000;"
        "000000000000001a;com.apple.app-sandbox.read;/var/mobile/Media/x.jpg") == 0);
    assert(minisandbox_token(token, sizeof(token), "com.apple.security.exception.mach-lookup.global-name", "") == 0);
    assert(strlen(token) < sizeof(token) && token[strlen(token) - 1] == ';');
    assert(minisandbox_token(token, 64, "com.apple.app-sandbox.read", "/x") == ENAMETOOLONG);
    /* third-party applications: profile "container" */
    assert(minisandbox_app_path("/private/var/containers/Bundle/Application/0B1C2D3E-0000-4000-8000-000000000001/Foo.app/Foo"));
    assert(minisandbox_app_path("/var/containers/Bundle/Application/0B1C2D3E-0000-4000-8000-000000000001/Foo.app/Foo"));
    assert(!minisandbox_app_path("/var/containers/Bundle/Application/"));
    assert(!minisandbox_app_path("/var/containers/Bundle/Application"));
    assert(!minisandbox_app_path("/var/containers/Bundle/Application//x"));
    assert(!minisandbox_app_path("/private/var/containers/Bundle/Applications/x/Foo.app/Foo"));
    assert(!minisandbox_app_path("/var/containers/Shared/SystemGroup/x"));
    assert(!minisandbox_app_path("/var/mobile/Containers/Data/Application/x/Documents/f"));
    assert(!minisandbox_app_path("/Applications/MobileSafari.app/MobileSafari"));
    assert(!minisandbox_app_path("/private/private/var/containers/Bundle/Application/x/Foo"));
    assert(!minisandbox_app_path("var/containers/Bundle/Application/x/Foo"));
    assert(!minisandbox_app_path(""));

    /* the log rate: the 1st, 2nd, 4th, 8th... */
    {
        unsigned logged = 0;
        for (unsigned n = 0; n <= 1000; n++) {
            logged += minisandbox_log_now(n);
        }
        assert(logged == 10 && minisandbox_log_now(512) && !minisandbox_log_now(0) && !minisandbox_log_now(3));
    }

    /* an application started by a sandboxed parent: inherited profile replaced by "container" */
    while (minisandbox_nprocs) {
        minisandbox_forget(minisandbox_procs[0].uniqueid);
    }
    assert(minisandbox_record(40, "springboard") == 0);
    assert(minisandbox_record_replace(40, "container") == EEXIST && strcmp(minisandbox_lookup(40)->profile, "container") == 0);
    assert(minisandbox_record_replace(41, "container") == 0 && strcmp(minisandbox_lookup(41)->profile, "container") == 0);
    while (minisandbox_nprocs < MINISANDBOX_MAX_PROCS) {
        assert(minisandbox_record(10000 + minisandbox_nprocs, "p") == 0);
    }
    assert(minisandbox_record_replace(9999, "container") == ENOSPC);
    assert(minisandbox_record_replace(40, "container") == EEXIST);

    /* request 6 and 0x1c blocks (sandbox_extension_consume, sandbox_reference_retain_by_audit_token) */
    assert(offsetof(struct minisandbox_consume_args, length) == 0x08);
    assert(offsetof(struct minisandbox_consume_args, handle) == 0x10);
    assert(offsetof(struct minisandbox_reference_args, pidversion) == 0x08);
    assert(offsetof(struct minisandbox_reference_args, reference) == 0x10);
    assert(MINISANDBOX_CHECK_BY_REFERENCE == 0xc0000000ULL);

    puts("minisandbox host test: ok");
    return 0;
}
'''

def c_string(text):
    return '"' + text.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n').replace('\t', '\\t') + '"'

program = '#define REAL_XML ' + c_string(real_xml) + '\n' + program
with tempfile.TemporaryDirectory() as tmp:
    c, exe = Path(tmp) / 'sandbox.c', Path(tmp) / 'sandbox'
    c.write_text(program)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                    '-fsanitize=address,undefined', '-o', str(exe), str(c)], check=True)
    subprocess.run([str(exe)], check=True)
