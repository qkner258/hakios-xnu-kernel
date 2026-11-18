#!/usr/bin/env python3
"""Add the two diagnostics that say who ends the init. Idempotent; exits 1 if an
anchor is missing so the workflow fails loudly instead of building an unpatched kernel."""
import sys

SRC = 'xnu-src/xnu-8796.121.2/bsd/kern'


def patch_sig():
    p = SRC + '/kern_sig.c'
    s = open(p).read()
    if 'hk-kill:' in s:
        return 'already'
    anchor = ('psignal_internal(proc_t p, task_t task, thread_t thread, int flavor, '
              'int signum, os_reason_t signal_reason)\n{\n')
    if anchor not in s:
        return 'anchor missing'
    add = (
        '    /* hk: only the init, and only when nothing else would say it */\n'
        '    if ((signum == SIGKILL || signum == SIGSTOP) && p != PROC_NULL && proc_getpid(p) == 1) {\n'
        '        proc_t hkme = current_proc();\n'
        '        printf("hk-kill: %s pid %d signum %d from %s pid %d\\n",\n'
        '            p->p_comm, proc_getpid(p), signum,\n'
        '            hkme != PROC_NULL ? hkme->p_comm : "?",\n'
        '            hkme != PROC_NULL ? proc_getpid(hkme) : -1);\n'
        '    }\n'
    )
    open(p, 'w').write(s.replace(anchor, anchor + add, 1))
    return 'patched'


def patch_exit():
    p = SRC + '/kern_exit.c'
    s = open(p).read()
    if 'hk-init:' in s:
        return 'already'
    anchor = 'exit_with_reason(proc_t p, int rv, int *retval, boolean_t thread_can_terminate'
    i = s.find(anchor)
    if i < 0:
        return 'anchor missing'
    b = s.find('{', i)
    if b < 0:
        return 'brace missing'
    add = ('\n    if (proc_pid(p) == 1) {\n'
           '        printf("hk-init: pid 1 exiting, status %d\\n", rv);\n'
           '    }\n')
    open(p, 'w').write(s[:b + 1] + add + s[b + 1:])
    return 'patched'


r1 = patch_sig()
r2 = patch_exit()
print('kern_sig.c:', r1)
print('kern_exit.c:', r2)
if 'missing' in r1 or 'missing' in r2:
    sys.exit(1)
