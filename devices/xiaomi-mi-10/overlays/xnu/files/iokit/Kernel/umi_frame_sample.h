#ifndef UMI_FRAME_SAMPLE_H
#define UMI_FRAME_SAMPLE_H
#include <stdint.h>

/* Fixed, sparse RGB sample: a changed sample proves a changed image; equal
 * samples do not prove equal images. No alpha, surface ID, or fence in the key.
 * Device/carve-out reads are volatile and naturally aligned 32-bit accesses. */
#define UMI_SAMPLE_COLS 32
#define UMI_SAMPLE_ROWS 64
#define UMI_SAMPLE_PIXELS (UMI_SAMPLE_COLS * UMI_SAMPLE_ROWS)
static inline void
umi_frame_sample(const volatile uint8_t *base, uint32_t width, uint32_t height,
                 uint32_t stride, uint32_t out[UMI_SAMPLE_PIXELS])
{
    for (unsigned y = 0; y < UMI_SAMPLE_ROWS; ++y) {
        unsigned py = (2 * y + 1) * height / (2 * UMI_SAMPLE_ROWS);
        const volatile uint32_t *row = (const volatile uint32_t *)(base + (uint64_t)py * stride);
        for (unsigned x = 0; x < UMI_SAMPLE_COLS; ++x) {
            unsigned px = (2 * x + 1) * width / (2 * UMI_SAMPLE_COLS);
            out[y * UMI_SAMPLE_COLS + x] = row[px] & 0x00ffffffu;
        }
    }
}
static inline unsigned
umi_frame_sample_changes(const uint32_t a[UMI_SAMPLE_PIXELS], const uint32_t b[UMI_SAMPLE_PIXELS])
{
    unsigned n = 0;
    for (unsigned i = 0; i < UMI_SAMPLE_PIXELS; ++i) n += a[i] != b[i];
    return n;
}
#endif
