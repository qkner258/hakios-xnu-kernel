/*
 * Where the panel's scan is (bsd/dev/hakios.display.c, boot-arg fb_scan=1): for the framebuffer driver
 * (iokit/Kernel/MiniIOMobileFramebuffer.cpp), so that frames go into the panel's memory where the scan lets
 * them land whole, at the panel's own rate.
 */
#ifndef _SYS_HK_DISPLAY_H_
#define _SYS_HK_DISPLAY_H_

#include <stdint.h>
#include <mach/vm_types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct hakios_display_scan {
	int ok;
	uint32_t lines;                 /* lines a frame, the blanking included (2432) */
	uint32_t shown;                 /* of which shown (2400): lines 0..shown-1, then the blanking */
	uint32_t millihz;               /* frames a second, in thousandths (119930) */
	uint64_t line_ns, frame_ns;
};

int hakios_display_scan_start(void);                           /* 1 when the scan can be read */
int hakios_display_session_prepare(void);                      /* once, before the first framebuffer client */
const struct hakios_display_scan *hakios_display_scan(void);  /* NULL without */
uint32_t hakios_display_line(uint32_t *frame);                 /* the line being sent now (and the frame's number) */
int hakios_display_wait_lines(uint32_t first, uint32_t last, uint64_t limit_ns);
uint64_t hakios_display_next_frame_ns(void);                   /* until the next frame's first line */
int hakios_display_command_mode(void);
int hakios_display_wait_copy(uint64_t copy_ns, uint64_t limit_ns);
uint32_t hakios_display_copy_nowin(void);                      /* copy window checks that found none (run total) */
uint32_t hakios_display_copy_impossible(void);                 /* ... and the ones that never could, not waited for */
int hakios_display_inplace_demote(void);                       /* fb_dpu=4 (a2): 0 with /var/db/hakios-display/inplace-demote-off */
int hakios_display_compose_copies(void);                       /* fb_dpu=4: 0 with /var/db/hakios-display/compose-off */
int hakios_display_write_kick(void);                           /* fb_dpu=4: 0 with /var/db/hakios-display/write-kick-off */
int hakios_display_write_shown(void);                          /* fb_dpu=4: 1 = only a latched successor frees the shown frame (0 with .../write-shown-off) */
/* fb_dpu=1: flip the UEFI pipes between frames of the splash carve-out (DISPLAY-SCANOUT-umi.md) */
unsigned hakios_display_dpu_start(uint64_t phys, uint32_t row_bytes, uint32_t height, vm_offset_t *virt, unsigned max);
int hakios_display_dpu_flip(unsigned index, uint64_t limit_ns);
int hakios_display_dpu_refresh(void);   /* fetch the shown frame once more (start a transfer for a copy in place) */
int hakios_display_dpu_poll(void);      /* the pending buffer, -1 when none */
int hakios_display_dpu_shown(void);
uint64_t hakios_display_dpu_frame(unsigned index);     /* physical address of a scanout frame */
void hakios_display_dpu_stride(unsigned index, uint32_t stride);
int hakios_display_dpu_leaving(unsigned index);        /* fb_dpu=4: shown, a flip away pending, still fetched */
uint32_t hakios_display_dpu_latched(void);
void hakios_display_dpu_restore(void);
void hakios_display_dpu_stop(void);
int hakios_display_dpu_fetch_test(uint64_t phys, vm_offset_t virt);      /* fb_dpu=2 */
unsigned hakios_display_dpu_scale_start(uint32_t w, uint32_t h, uint32_t stride, vm_offset_t *virt, unsigned max); /* fb_dpu=3 */

#ifdef __cplusplus
}
#endif

#endif /* _SYS_HK_DISPLAY_H_ */
