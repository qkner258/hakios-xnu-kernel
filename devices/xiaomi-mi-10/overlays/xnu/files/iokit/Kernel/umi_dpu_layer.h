/* Kernel display compositor contract; no GPU submission or userland ABI changes. */
#ifndef UMI_DPU_LAYER_H
#define UMI_DPU_LAYER_H
#include <stdint.h>
#define UMI_DPU_SOURCE_BYTES (12u * 1024u * 1024u)
struct umi_dpu_layer {
	uint32_t width, height, stride;
	int32_t src[4], dst[4];
	uint32_t rgba;                  /* 0 BGRA, 1 RGBA; overlays are premultiplied */
};
#ifdef __cplusplus
extern "C" {
#endif
int hakios_display_compose_start(void);
int hakios_display_compose_begin(uint64_t limit_ns); /* free snapshot bank, after previous latch; -1 on failure */
uintptr_t hakios_display_compose_source(unsigned bank, unsigned layer);
int hakios_display_compose_validate(const struct umi_dpu_layer *layers, unsigned count);
int hakios_display_compose(unsigned bank, const struct umi_dpu_layer *layers, unsigned count);
#ifdef __cplusplus
}
#endif
#endif
