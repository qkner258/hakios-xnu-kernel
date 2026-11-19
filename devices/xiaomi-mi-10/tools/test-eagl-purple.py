#!/usr/bin/env python3
"""Check the actual pre-draw probe with a mock GL; no GPU, phone or GL context."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[3]
source = (root / 'devices/xiaomi-mi-10/overlays/userland/src/eagl.c').read_text()
start = source.index('static const char *\nblend_name(')
end = source.index('/* From inject.c: CoreAnimation asked whether', start)
shim = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
typedef unsigned GLenum, GLuint;
typedef int GLint, GLsizei;
typedef float GLfloat;
#define GL_DRAW_FRAMEBUFFER 0x8ca9
static int purple_fix = -1, finishes, inspections, program = 7;
static const char *shader = "#version 300 es\nout vec4 result;";
static void *ca_context;
struct eagl {
 struct { GLuint name; bool known, fetch; } fetch_programs[256];
 uint64_t diagnostic_draws[256];
 unsigned diagnostic_count, fetch_finishes, frames, program, framebuffer, colour, bound[8];
};
static const char *census_name(unsigned f) { return "???"; }
static void say(const char *fmt, ...) {}
static void glGetIntegerv(GLenum n, GLint *v) { *v = n == 0x8b8d ? program : 0x8006; }
static int glIsEnabled(GLenum n) { return 1; }
static void glGetAttachedShaders(GLuint p, GLsizei max, GLsizei *n, GLuint *s) { *n = 1; *s = 1; }
static void glGetShaderiv(GLuint s, GLenum n, GLint *v) { *v = strlen(shader) + 1; }
static void glGetShaderSource(GLuint s, GLsizei n, GLsizei *len, char *text) {
 inspections++; snprintf(text, n, "%s", shader);
}
static void glGetFloatv(GLenum n, GLfloat *v) { memset(v, 0, 4*sizeof(*v)); }
static GLenum glCheckFramebufferStatus(GLenum n) { return 0x8cd5; }
static void glFinish(void) { finishes++; }
'''
test = r'''
int main(void) {
 struct eagl e = {0};
 purple_before_draw(&e);
 assert(!finishes && !inspections); /* absent boot arg does no GL work */
 purple_fix = 0;
 purple_before_draw(&e);
 assert(!finishes && inspections == 1 && e.diagnostic_count == 1);
 purple_before_draw(&e);
 assert(!finishes && inspections == 1 && e.diagnostic_count == 1); /* cached/deduplicated */
 purple_fix = 1;
 purple_before_draw(&e);
 assert(!finishes); /* ordinary shader must not stall */
 shader = "#extension GL_EXT_shader_framebuffer_fetch : require\nlayout(location=0) inout vec4 result;";
 e.fetch_programs[7].known = false; /* same invalidation done by link/delete wrappers */
 purple_before_draw(&e);
 assert(finishes == 1 && inspections == 2);
 purple_before_draw(&e);
 assert(finishes == 2 && inspections == 2); /* every fetch draw, not only transitions */
 purple_fix = 0;
 purple_before_draw(&e);
 assert(finishes == 2); /* diagnostics preserves draw submission */
 purple_fix = 1;
 program = 263; shader = "out vec4 result;"; /* hash slot collision */
 purple_before_draw(&e);
 assert(finishes == 2 && inspections == 3);
 program = 7; shader = "#extension GL_EXT_shader_framebuffer_fetch : require\ninout vec4 result;";
 e.diagnostic_count = 256; /* logging cap cannot disable the workaround */
 purple_before_draw(&e);
 assert(finishes == 3 && inspections == 4);
 program = 0;
 purple_before_draw(&e);
 assert(finishes == 3 && inspections == 4);
 return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    c = Path(directory) / 'probe.c'
    exe = Path(directory) / 'probe'
    c.write_text(shim + source[start:end] + test)
    subprocess.run(['cc', '-std=c11', '-Wno-unused-parameter', '-fsanitize=address,undefined',
                    str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('PASS: opt-in fetch serialization, ordinary draws, diagnostic-only mode, cache collisions and census cap')
