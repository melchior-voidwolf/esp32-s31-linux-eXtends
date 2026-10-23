/* fbaccel.h — ESP32-S31 PPA/DMA2D framebuffer acceleration library.
 *
 * Userspace /dev/mem driver for the S31's 2D-DMA (DMA2D) + PPA blocks.
 * All the S31 silicon quirks found during bring-up are baked in here:
 *   - physical addresses into link registers, descriptors in internal SRAM
 *   - 64B bursts minimum when touching PSRAM (32B trips fatal MSPI errors)
 *   - PPA fill needs the blend_trans_mode_update WT pulse
 *   - SRM needs a full CLKRST reset before EVERY operation
 *   - fixed channel plan: SRM=TX0/RX0, blend=TX1/TX2/RX2, fill=RX1, M2M=ch0
 *     (SRM's dscr-port mode taints ch0: TX wedges in state-2 and RX0
 *      misses suc_eof for later non-dscr-port users)
 *   - blend_fix_alpha must be written as one 32-bit .val store (reserved
 *     bits read as 1; RMW turns alpha inversion on -> A'=0 -> garbage)
 *
 * API:
 *   fbaccel_init()          map + bring up DMA2D/PPA, bind the fb
 *   fbaccel_fill(x,y,w,h,c)          PPA solid fill   ~210 MB/s
 *   fbaccel_blit(dx,dy,sx,sy,w,h)    M2M copy         ~106 MB/s
 *   fbaccel_srm(...)                 scale/rot/mirror ~120 MB/s
 *   fbaccel_blend(...)               alpha blend      ~209 MB/s
 *   fbaccel_close()
 */
#ifndef FBACCEL_H
#define FBACCEL_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    int fd_mem;
    int fd_fb;
    uint16_t *fb;            /* cached user mapping */
    uint32_t fb_phys;
    uint32_t fb_len;
    uint32_t stride;         /* bytes per line */
    uint32_t width, height;
    void *dma2d;
    void *ppa;
    void *clkrst;
    void *hpsys;
    void *cache;
    void *desc;              /* internal-SRAM descriptor page (cached view) */
    uint32_t desc_phys;
} fbaccel_t;

/* Open /dev/fb0 (or FBACCEL_FB env), map everything, power up the engines.
 * Returns 0 on success. */
int fbaccel_init(fbaccel_t *fa);

/* Solid-fill a rectangle in the framebuffer (RGB565 color). */
int fbaccel_fill(fbaccel_t *fa, uint32_t x, uint32_t y,
                 uint32_t w, uint32_t h, uint16_t color);

/* Copy a rectangle within the framebuffer (may overlap? no — same fb,
 * non-overlapping windows only; the DMA runs line-by-line so small
 * overlaps downwards are safe, large ones are not). */
int fbaccel_blit(fbaccel_t *fa, uint32_t dx, uint32_t dy,
                 uint32_t sx, uint32_t sy, uint32_t w, uint32_t h);

/* Scale/rotate/mirror: src window -> dst window.
 * angle: 0/1/2/3 = 0/90/270-CCW/180 degrees (PPA encoding).
 * scale 0.5 = int 0 frag 8 (frag is /16). mirror_x/y flip in dst.
 * dst w/h = src w/h scaled (swapped for 90/270). */
int fbaccel_srm(fbaccel_t *fa,
                uint32_t sx, uint32_t sy, uint32_t sw, uint32_t sh,
                uint32_t dx, uint32_t dy,
                uint32_t x_int, uint32_t x_frag,
                uint32_t y_int, uint32_t y_frag,
                uint32_t angle, uint32_t mirror_x, uint32_t mirror_y);

/* Alpha-blend two source windows into a destination window.
 * alpha_fg/alpha_bg: 0-255 fixed alpha per plane. */
int fbaccel_blend(fbaccel_t *fa,
                  uint32_t bg_x, uint32_t bg_y,
                  uint32_t fg_x, uint32_t fg_y,
                  uint32_t dx, uint32_t dy,
                  uint32_t w, uint32_t h,
                  uint32_t alpha_fg, uint32_t alpha_bg);

/* CPU-side cache ops (exported for callers that DMA elsewhere). */
void fbaccel_wb(fbaccel_t *fa, const void *p, size_t len);
void fbaccel_wb_inv(fbaccel_t *fa, void *p, size_t len);

void fbaccel_close(fbaccel_t *fa);

#endif
