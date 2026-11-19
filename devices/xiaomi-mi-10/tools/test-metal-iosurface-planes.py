#!/usr/bin/env python3
"""Run the CA plug-in's actual shadow methods against two mocked IOSurface planes."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[3]
source = (root / 'userland/src/HakiosMetalCA.m').read_text()
def method(start, end):
    at = source.index(start + '\n{')
    return source[at:source.index(end, at)]
methods = method('- (void)hakiosWriteBackShadow', '/* A \'420f\'')
methods += method('- (void)hakiosRefreshShadow', '- (void)hakiosSetIOSurface:')
shim = r'''
#import <Foundation/Foundation.h>
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
typedef struct __IOSurface { uint8_t data[8192]; uint32_t seed; } Surface;
typedef struct { unsigned width, height, cpp; uint8_t data[8192]; } Texture;
enum { MTLTextureUsageShaderRead = 1, MTLTextureUsageShaderWrite = 2, MTLTextureUsageRenderTarget = 4,
       kIOSurfaceLockReadOnly = 1 };
static size_t IOSurfaceGetPlaneCount(Surface *s) { return 2; }
static size_t IOSurfaceGetBytesPerRowOfPlane(Surface *s, NSUInteger p) { return p ? 816 : 800; }
static size_t IOSurfaceGetBytesPerRow(Surface *s) { return 800; }
static void *IOSurfaceGetBaseAddress(Surface *s) { return s->data; }
static void *IOSurfaceGetBaseAddressOfPlane(Surface *s, NSUInteger p) { return s->data + (p ? 4992 : 128); }
static uint32_t IOSurfaceGetSeed(Surface *s) { return s->seed; }
static unsigned IOSurfaceGetID(Surface *s) { return 1; }
static int IOSurfaceGetUseCount(Surface *s) { return 0; }
static BOOL IOSurfaceIsInUse(Surface *s) { return NO; }
static int IOSurfaceLock(Surface *s, unsigned flags, uint32_t *seed) { if (seed) *seed=s->seed; return 0; }
static int IOSurfaceUnlock(Surface *s, unsigned flags, uint32_t *seed) { if (!flags) ++s->seed; return 0; }
static unsigned hakios_metal_texture_width(Texture *t) { return t->width; }
static unsigned hakios_metal_texture_height(Texture *t) { return t->height; }
static int hakios_metal_texture_replace(Texture *t, unsigned x, unsigned y, unsigned w, unsigned h, const void *src, uint64_t pitch) {
    assert(w == t->width && h == t->height && w*t->cpp <= pitch);
    for (unsigned row=0; row<h; ++row) memcpy(t->data+row*w*t->cpp, (const uint8_t *)src+row*pitch, w*t->cpp);
    return 0;
}
static int hakios_metal_texture_read(Texture *t, unsigned x, unsigned y, unsigned w, unsigned h, void *dst, uint64_t pitch) {
    assert(w == t->width && h == t->height && w*t->cpp <= pitch);
    for (unsigned row=0; row<h; ++row) memcpy((uint8_t *)dst+row*pitch, t->data+row*w*t->cpp, w*t->cpp);
    return 0;
}
static int hm_ca_arg(const char *arg) { return 0; }
static double hm_now(void) { return 0; }
static void hm_fps_shadow(BOOL in, double elapsed) {}
static void hm_fps_ycbcr_time(double elapsed) {}
static void hm_print(const char *fmt, ...) {}
static BOOL hm_handoff_on(void) { return NO; }
static struct { unsigned shadowImports; } hm_diag;
#define hm_diag_add(counter) (*(counter) += 1)
static BOOL hm_flicker_say(unsigned *count) { return NO; }
static size_t IOSurfaceGetWidth(Surface *s) { return 200; }
static size_t IOSurfaceGetHeight(Surface *s) { return 8; }
@interface HakiosMetalTexture : NSObject {
@public
    HakiosMetalTexture *_parent;
    BOOL _shadow;
    IOSurfaceRef _iosurface;
    NSUInteger _iosurfacePlane, _usage;
    uint8_t *_shadowBase;
    uint32_t _shadowSeed;
    unsigned _shadowWrites;
    NSLock *_shadowLock;
    int _ycbcr;
    Texture *_texture;
}
- (void)hakiosSetIOSurface:(IOSurfaceRef)s plane:(NSUInteger)p;
- (void)hakiosShadow:(IOSurfaceRef)s;
- (void)hakiosShadow:(IOSurfaceRef)s plane:(NSUInteger)p;
- (void)hakiosRefreshShadow;
- (void)hakiosWriteBackShadow;
- (BOOL)hakiosConvertYCbCr;
@end
@implementation HakiosMetalTexture
- (void)hakiosSetIOSurface:(IOSurfaceRef)s plane:(NSUInteger)p { _iosurface=s; _iosurfacePlane=p; }
- (BOOL)hakiosConvertYCbCr { assert(0); return NO; }
'''
test = r'''
@end
int main(void) {
 @autoreleasepool {
    for (unsigned packed=0; packed<2; ++packed) {
        Surface s; memset(&s, 0x5a, sizeof s); s.seed=3;
        for (unsigned i=0; i<788*6; ++i) s.data[128+(i/788)*800+i%788] = i%251;
        for (unsigned i=0; i<788*3; ++i) s.data[4992+(i/788)*816+i%788] = (i+73)%251;
        uint8_t expected[8192]; memcpy(expected, s.data, sizeof expected);
        Texture yt={.width=packed ? 197 : 788, .height=6, .cpp=packed ? 4 : 1};
        Texture uvt={.width=packed ? 197 : 394, .height=3, .cpp=packed ? 4 : 2};
        HakiosMetalTexture *y=[HakiosMetalTexture new], *uv=[HakiosMetalTexture new];
        y->_texture=&yt; uv->_texture=&uvt; y->_usage=uv->_usage=MTLTextureUsageShaderRead|MTLTextureUsageShaderWrite;
        [y hakiosShadow:&s plane:0]; [uv hakiosShadow:&s plane:1];
        assert(y->_iosurfacePlane==0 && uv->_iosurfacePlane==1);
        assert(yt.data[787+5*788]==s.data[128+787+5*800]);
        assert(uvt.data[787+2*788]==s.data[4992+787+2*816]);
        yt.data[17] ^= 0xff; expected[128+17] ^= 0xff;
        uvt.data[2*788+787] ^= 0xff; expected[4992+2*816+787] ^= 0xff;
        [y hakiosWriteBackShadow]; [uv hakiosWriteBackShadow];
        assert(memcmp(s.data, expected, sizeof expected)==0); // all guards, padding and other plane preserved
        s.data[4992+816+3]^=0x80; ++s.seed;
        [uv hakiosRefreshShadow];
        assert(uvt.data[788+3]==s.data[4992+816+3] && uv->_shadowSeed==s.seed);
        [uv hakiosWriteBackShadow];
        assert(yt.data[17]==s.data[128+17]);
        free(y->_shadowBase); free(uv->_shadowBase); [y release]; [uv release];
    }
 }
 puts("PASS: actual CA shadow methods, R8/RG8 and packed RGBA8, plane offsets/pitches, seed refresh, isolated writeback");
 return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory) / 'planes.m'
    exe = Path(directory) / 'planes'
    path.write_text(shim + methods + test)
    subprocess.run(['xcrun', 'clang', '-fno-objc-arc', '-fsanitize=address,undefined',
                    '-Werror', '-Wno-unused-function', '-framework', 'Foundation', str(path), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
