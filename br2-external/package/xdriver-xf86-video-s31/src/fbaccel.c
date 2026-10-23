/* fbaccel.c — ESP32-S31 PPA/DMA2D framebuffer acceleration library.
 * Register recipes proven in ppa-work/ppa-test/ppa_test.c v36 (all-mode
 * green: fill 210 MB/s, blit 106 MB/s, SRM 121 MB/s, blend 209 MB/s).
 */
#include "fbaccel.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/fb.h>

#include "dma2d_struct.h"
#include "ppa_struct.h"

#define PPA_BASE    0x20345000u
#define DMA2D_BASE  0x20346000u
#define CLKRST_BASE 0x20587000u
#define HPSYS_BASE  0x20586000u
#define CACHE_BASE  0x2C000000u
#define DESC_BASE   0x2f04f000u   /* internal SRAM page reserved for us: the
                                   * kernel dma-pool ends at 0x2f04f000 (see
                                   * esp32s31.dtsi), so this page is ours alone */

#define PERI_PPA_SRM_TX      1
#define PERI_PPA_SRM_RX      1
#define PERI_PPA_BLEND_RX    2
#define PERI_PPA_BLEND_BG_TX 2
#define PERI_PPA_BLEND_FG_TX 3
#define PERI_M2M_RX          3
#define PERI_M2M_TX          4

#define IN_INT_SUC_EOF  (1u << 1)
#define IN_INT_ALL_ERR  ((1u << 2) | (1u << 3))
#define DSCR_DMA2D_EN   (1u << 29)
#define DSCR_SUC_EOF    (1u << 30)
#define DSCR_OWNER      (1u << 31)

#define R32(p)      (*(volatile uint32_t *)(p))
#define W32(p, v)   (*(volatile uint32_t *)(p) = (v))

/* descriptor slots inside the SRAM page (24 B each, 64 B apart) */
#define OFF_FILL  0x00u
#define OFF_TX    0x40u
#define OFF_RX    0x80u
#define OFF_FG    0xc0u

typedef struct {
    uint32_t w0, w1, w2;
    void *buf, *next;
} fdesc_t;

/* ---------- low-level helpers ---------- */

static void cache_op(fbaccel_t *fa, void *vaddr, uint32_t size, uint32_t op_bit)
{
    volatile uint32_t *c = fa->cache;
    uint32_t va = (uint32_t)(uintptr_t)vaddr & ~31u;
    uint32_t n = (size + 31u) & ~31u;
    W32(c + 0xa0 / 4, 0x1fu);
    W32(c + 0xa4 / 4, va);
    W32(c + 0xa8 / 4, n);
    W32(c + 0x9c / 4, op_bit);
    while (R32(c + 0x9c / 4) & op_bit)
        ;
}

void fbaccel_wb(fbaccel_t *fa, const void *p, size_t len)
{
    cache_op(fa, (void *)p, len, 1u << 2);
}

void fbaccel_wb_inv(fbaccel_t *fa, void *p, size_t len)
{
    cache_op(fa, (void *)p, len, 1u << 3);
}

static void desc_set_2d(fdesc_t *d, uint32_t bw, uint32_t bh,
                        uint32_t pw, uint32_t ph, uint32_t x, uint32_t y,
                        uint32_t pbyte, void *buf)
{
    d->w0 = (bh & 0x3fff) | ((bw & 0x3fff) << 14)
          | DSCR_DMA2D_EN | DSCR_SUC_EOF | DSCR_OWNER;
    d->w1 = (ph & 0x3fff) | ((pw & 0x3fff) << 14) | ((pbyte & 0xf) << 28);
    d->w2 = (y & 0x3fff) | ((x & 0x3fff) << 14);
    d->buf = buf;
    d->next = NULL;
}

/* wait for RX suc_eof OR a PPA engine eof bit (ppaeof_mask); the S31
 * sometimes swallows the RX eof after channel-mode changes — the engine
 * eof flag is then the authoritative completion. timeout in µs, ~10
 * MMIO-read spins per µs at 320 MHz. */
static int wait_done(fbaccel_t *fa, uint32_t rx_ch, uint32_t ppaeof_mask,
                     uint64_t timeout_us)
{
    volatile dma2d_dev_t *dma = fa->dma2d;
    volatile ppa_dev_t *ppa = fa->ppa;
    uint64_t spins = timeout_us * 2u;   /* each iter = several MMIO reads
                    (~0.5µs); keep failures short but decisive */
    for (volatile uint64_t i = 0; i < spins; i++) {
        if (R32(&dma->in_channel[rx_ch].in_int_raw) & IN_INT_SUC_EOF) {
            W32(&dma->in_channel[rx_ch].in_int_clr, IN_INT_SUC_EOF);
            return 1;
        }
        if (ppaeof_mask && (R32(&ppa->int_raw) & ppaeof_mask)) {
            W32(&ppa->int_clr, ppaeof_mask);
            return 1;
        }
        if (R32(&dma->in_channel[rx_ch].in_int_raw) & IN_INT_ALL_ERR)
            return 0;
    }
    return 0;
}

/* CLKRST-level PPA reset + the state that does not survive it */
static void ppa_hard_reset(fbaccel_t *fa)
{
    volatile uint32_t *ck = fa->clkrst;
    volatile uint32_t *hp = fa->hpsys;
    volatile ppa_dev_t *ppa = fa->ppa;
    ck[0xb4 / 4] |= 2u;
    ck[0xb4 / 4] &= ~2u;
    hp[0x29c / 4] |= (1u << 3);
    hp[0x29c / 4] &= ~(1u << 2);
    ppa->reg_conf.clk_en = 1;
    ppa->srm_mem_pd.srm_mem_clk_ena = 1;
    ppa->srm_mem_pd.srm_mem_force_pd = 0;
    ppa->srm_mem_pd.srm_mem_force_pu = 1;
}

/* ---------- init / close ---------- */

int fbaccel_init(fbaccel_t *fa)
{
    memset(fa, 0, sizeof(*fa));

    const char *fbdev = "/dev/fb0";
    fa->fd_fb = open(fbdev, O_RDWR);
    if (fa->fd_fb < 0)
        return -1;
    struct fb_fix_screeninfo finfo;
    struct fb_var_screeninfo vinfo;
    ioctl(fa->fd_fb, FBIOGET_FSCREENINFO, &finfo);
    ioctl(fa->fd_fb, FBIOGET_VSCREENINFO, &vinfo);
    if (vinfo.bits_per_pixel != 16 || !finfo.smem_start) {
        close(fa->fd_fb);
        return -1;
    }
    fa->fb_phys = finfo.smem_start;
    fa->fb_len = finfo.smem_len;
    fa->stride = finfo.line_length;
    fa->width = vinfo.xres;
    fa->height = vinfo.yres;
    fa->fb = mmap(NULL, fa->fb_len, PROT_READ | PROT_WRITE, MAP_SHARED,
                  fa->fd_fb, 0);
    if (fa->fb == MAP_FAILED) {
        close(fa->fd_fb);
        return -1;
    }

    fa->fd_mem = open("/dev/mem", O_RDWR | O_SYNC);
    if (fa->fd_mem < 0)
        return -1;
    #define MAP4(base, len) mmap(NULL, len, PROT_READ | PROT_WRITE, \
                                 MAP_SHARED, fa->fd_mem, base)
    fa->dma2d = MAP4(DMA2D_BASE, 0x1000);
    fa->ppa = MAP4(PPA_BASE, 0x1000);
    fa->clkrst = MAP4(CLKRST_BASE, 0x1000);
    fa->hpsys = MAP4(HPSYS_BASE, 0x1000);
    fa->cache = MAP4(CACHE_BASE, 0x1000);
    fa->desc = MAP4(DESC_BASE, 0x1000);
    fa->desc_phys = DESC_BASE;
    if (fa->dma2d == MAP_FAILED || fa->ppa == MAP_FAILED ||
        fa->clkrst == MAP_FAILED || fa->hpsys == MAP_FAILED ||
        fa->cache == MAP_FAILED || fa->desc == MAP_FAILED)
        return -1;

    volatile dma2d_dev_t *dma = fa->dma2d;
    volatile ppa_dev_t *ppa = fa->ppa;
    volatile uint32_t *ck = fa->clkrst;
    volatile uint32_t *hp = fa->hpsys;

    /* DMA2D bring-up (order matters): bus clk, reset pulse, memory power */
    ck[0xb0 / 4] |= 1u;
    ck[0xb0 / 4] |= 2u;
    ck[0xb0 / 4] &= ~2u;
    hp[0x210 / 4] &= ~(1u << 2);
    hp[0x210 / 4] &= ~(1u << 3);
    dma->rst_conf.clk_en = 1;
    dma->rst_conf.axim_rd_rst = 1;
    dma->rst_conf.axim_rd_rst = 0;
    dma->rst_conf.axim_wr_rst = 1;
    dma->rst_conf.axim_wr_rst = 0;
    dma->out_arb_config.val = 0;
    dma->in_arb_config.val = 0;
    dma->intr_mem_start_addr.val = 0;
    dma->intr_mem_end_addr.val = 0xFFFFFFFFu;
    dma->extr_mem_start_addr.val = 0;
    dma->extr_mem_end_addr.val = 0xFFFFFFFFu;

    /* PPA bring-up + SRM data memory power */
    ck[0xb4 / 4] |= 1u;
    ck[0xb4 / 4] |= 2u;
    ck[0xb4 / 4] &= ~2u;
    hp[0x29c / 4] |= (1u << 3);
    hp[0x29c / 4] &= ~(1u << 2);
    ppa->reg_conf.clk_en = 1;
    ppa->srm_mem_pd.srm_mem_clk_ena = 1;
    ppa->srm_mem_pd.srm_mem_force_pd = 0;
    ppa->srm_mem_pd.srm_mem_force_pu = 1;

    memset(fa->desc, 0, 0x100);
    return 0;
}

void fbaccel_close(fbaccel_t *fa)
{
    if (fa->fb) munmap(fa->fb, fa->fb_len);
    if (fa->fd_fb >= 0) close(fa->fd_fb);
    if (fa->fd_mem >= 0) close(fa->fd_mem);
    memset(fa, 0, sizeof(*fa));
}

/* ---------- fill (PPA fix-pixel mode, RX ch1) ---------- */

int fbaccel_fill(fbaccel_t *fa, uint32_t x, uint32_t y,
                 uint32_t w, uint32_t h, uint16_t color)
{
    volatile dma2d_dev_t *dma = fa->dma2d;
    volatile ppa_dev_t *ppa = fa->ppa;
    fdesc_t *d = (fdesc_t *)((uint8_t *)fa->desc + OFF_FILL);
    uint32_t dphys = fa->desc_phys + OFF_FILL;
    const uint32_t ch = 1;

    desc_set_2d(d, w, h, fa->width, fa->height, x, y, 3, (void *)fa->fb_phys);
    /* the desc page lives in internal SRAM; /dev/mem maps it uncached, so
     * CPU writes reach the DMA directly (as proven in ppa_test v36) */

    dma->in_channel[ch].in_conf0.in_rst_chn = 1;
    dma->in_channel[ch].in_conf0.in_rst_chn = 0;
    dma->in_channel[ch].in_peri_sel.in_peri_sel_chn = PERI_PPA_BLEND_RX;
    dma->in_channel[ch].in_conf0.indscr_burst_en_chn = 1;
    dma->in_channel[ch].in_conf0.in_mem_burst_length_chn = 3;
    dma->in_channel[ch].in_conf0.in_macro_block_size_chn = 3;
    dma->in_channel[ch].in_conf0.in_page_bound_en_chn = 1;

    ppa->blend_trans_mode.blend_rst = 1;
    ppa->blend_trans_mode.blend_rst = 0;
    ppa->blend_fix_pixel.blend_tx_fix_pixel = color;
    ppa->blend_color_mode.blend_tx_cm = 2;
    ppa->blend_tx_size.blend_hb = w;
    ppa->blend_tx_size.blend_vb = h;

    dma->in_channel[ch].in_link_addr.inlink_addr_chn = dphys;
    dma->in_channel[ch].in_link_conf.inlink_start_chn = 1;

    ppa->blend_trans_mode.blend_bypass = 0;
    ppa->blend_trans_mode.blend_fix_pixel_fill_en = 1;
    ppa->blend_trans_mode.blend_en = 1;
    ppa->blend_trans_mode.blend_trans_mode_update = 1;   /* WT latch */

    if (!wait_done(fa, ch, 2u /*blend_eof*/, 200000))
        return -1;
    fbaccel_wb_inv(fa, (uint8_t *)fa->fb + (size_t)y * fa->stride,
                   (size_t)h * fa->stride);
    return 0;
}

/* ---------- blit (M2M, ch0 pair) ---------- */

int fbaccel_blit(fbaccel_t *fa, uint32_t dx, uint32_t dy,
                 uint32_t sx, uint32_t sy, uint32_t w, uint32_t h)
{
    volatile dma2d_dev_t *dma = fa->dma2d;
    fdesc_t *dtx = (fdesc_t *)((uint8_t *)fa->desc + OFF_TX);
    fdesc_t *drx = (fdesc_t *)((uint8_t *)fa->desc + OFF_RX);
    uint32_t ptx = fa->desc_phys + OFF_TX;
    uint32_t prx = fa->desc_phys + OFF_RX;

    desc_set_2d(dtx, w, h, fa->width, fa->height, sx, sy, 3,
                (void *)fa->fb_phys);
    desc_set_2d(drx, w, h, fa->width, fa->height, dx, dy, 3,
                (void *)fa->fb_phys);

    dma->out_channel[0].out_conf0.out_rst_chn = 1;
    dma->out_channel[0].out_conf0.out_rst_chn = 0;
    dma->out_channel[0].out_peri_sel.out_peri_sel_chn = PERI_M2M_TX;
    dma->out_channel[0].out_conf0.outdscr_burst_en_chn = 1;
    dma->out_channel[0].out_conf0.out_mem_burst_length_chn = 3;
    dma->out_channel[0].out_conf0.out_macro_block_size_chn = 3;
    dma->out_channel[0].out_conf0.out_page_bound_en_chn = 1;
    dma->out_channel[0].out_conf0.out_dscr_port_en_chn = 0;

    dma->in_channel[0].in_conf0.in_rst_chn = 1;
    dma->in_channel[0].in_conf0.in_rst_chn = 0;
    dma->in_channel[0].in_peri_sel.in_peri_sel_chn = PERI_M2M_RX;
    dma->in_channel[0].in_conf0.in_mem_trans_en_chn = 1;
    dma->in_channel[0].in_conf0.indscr_burst_en_chn = 1;
    dma->in_channel[0].in_conf0.in_mem_burst_length_chn = 3;
    dma->in_channel[0].in_conf0.in_macro_block_size_chn = 3;
    dma->in_channel[0].in_conf0.in_page_bound_en_chn = 1;
    dma->in_channel[0].in_conf0.in_dscr_port_en_chn = 0;

    fbaccel_wb(fa, (uint8_t *)fa->fb + (size_t)sy * fa->stride,
               (size_t)h * fa->stride);
    dma->out_channel[0].out_link_addr.outlink_addr_chn = ptx;
    dma->in_channel[0].in_link_addr.inlink_addr_chn = prx;
    dma->in_channel[0].in_link_conf.inlink_start_chn = 1;
    dma->out_channel[0].out_link_conf.outlink_start_chn = 1;

    if (!wait_done(fa, 0, 0, 200000))
        return -1;
    fbaccel_wb_inv(fa, (uint8_t *)fa->fb + (size_t)dy * fa->stride,
                   (size_t)h * fa->stride);
    return 0;
}

/* ---------- SRM (scale/rotate/mirror, TX0+RX0, dscr-port) ---------- */

int fbaccel_srm(fbaccel_t *fa,
                uint32_t sx, uint32_t sy, uint32_t sw, uint32_t sh,
                uint32_t dx, uint32_t dy,
                uint32_t x_int, uint32_t x_frag,
                uint32_t y_int, uint32_t y_frag,
                uint32_t angle, uint32_t mirror_x, uint32_t mirror_y)
{
    volatile dma2d_dev_t *dma = fa->dma2d;
    volatile ppa_dev_t *ppa = fa->ppa;
    fdesc_t *dtx = (fdesc_t *)((uint8_t *)fa->desc + OFF_TX);
    fdesc_t *drx = (fdesc_t *)((uint8_t *)fa->desc + OFF_RX);
    uint32_t ptx = fa->desc_phys + OFF_TX;
    uint32_t prx = fa->desc_phys + OFF_RX;
    uint32_t ow, oh;
    if (angle == 1 || angle == 3) {
        ow = y_int * sh + y_frag * sh / 16;
        oh = x_int * sw + x_frag * sw / 16;
    } else {
        ow = x_int * sw + x_frag * sw / 16;
        oh = y_int * sh + y_frag * sh / 16;
    }

    /* 1) full PPA reset — the SRM engine is single-shot otherwise */
    ppa_hard_reset(fa);
    W32(&ppa->int_clr, 0xFFFFFFFFu);
    W32(&dma->out_channel[0].out_int_clr, 0xFFFFFFFFu);
    W32(&dma->in_channel[0].in_int_clr, 0xFFFFFFFFu);

    /* 2) descriptors (RX gets the REAL output dims on S31) */
    desc_set_2d(dtx, sw, sh, fa->width, fa->height, sx, sy, 3,
                (void *)fa->fb_phys);
    desc_set_2d(drx, ow, oh, fa->width, fa->height, dx, dy, 3,
                (void *)fa->fb_phys);

    /* 3) channels: stop, reset, full reconfig (dscr-port mode) */
    dma->out_channel[0].out_link_conf.outlink_stop_chn = 1;
    dma->in_channel[0].in_link_conf.inlink_stop_chn = 1;
    dma->out_channel[0].out_conf0.out_rst_chn = 1;
    dma->out_channel[0].out_conf0.out_rst_chn = 0;
    dma->in_channel[0].in_conf0.in_rst_chn = 1;
    dma->in_channel[0].in_conf0.in_rst_chn = 0;

    dma->out_channel[0].out_peri_sel.out_peri_sel_chn = PERI_PPA_SRM_TX;
    dma->out_channel[0].out_conf0.outdscr_burst_en_chn = 1;
    dma->out_channel[0].out_conf0.out_mem_burst_length_chn = 3;
    dma->out_channel[0].out_conf0.out_macro_block_size_chn = 3;
    dma->out_channel[0].out_conf0.out_page_bound_en_chn = 1;
    dma->out_channel[0].out_conf0.out_dscr_port_en_chn = 1;
    dma->out_channel[0].out_dscr_port_blk.out_dscr_port_blk_h_chn = 34;
    dma->out_channel[0].out_dscr_port_blk.out_dscr_port_blk_v_chn = 34;

    dma->in_channel[0].in_peri_sel.in_peri_sel_chn = PERI_PPA_SRM_RX;
    dma->in_channel[0].in_conf0.in_mem_trans_en_chn = 0;
    dma->in_channel[0].in_conf0.indscr_burst_en_chn = 1;
    dma->in_channel[0].in_conf0.in_mem_burst_length_chn = 3;
    dma->in_channel[0].in_conf0.in_macro_block_size_chn = 3;
    dma->in_channel[0].in_conf0.in_page_bound_en_chn = 1;
    dma->in_channel[0].in_conf0.in_dscr_port_en_chn = 1;

    /* 4) arm + start */
    fbaccel_wb(fa, (uint8_t *)fa->fb + (size_t)sy * fa->stride,
               (size_t)sh * fa->stride);
    dma->out_channel[0].out_link_addr.outlink_addr_chn = ptx;
    dma->in_channel[0].in_link_addr.inlink_addr_chn = prx;
    dma->out_channel[0].out_link_conf.outlink_start_chn = 1;
    dma->in_channel[0].in_link_conf.inlink_start_chn = 1;

    ppa->srm_scal_rotate.scal_rotate_rst = 1;
    ppa->srm_scal_rotate.scal_rotate_rst = 0;
    ppa->srm_color_mode.srm_rx_cm = 2;
    ppa->srm_color_mode.srm_tx_cm = 2;
    ppa->srm_byte_order.srm_bk_size_sel = 0;
    ppa->srm_scal_rotate.srm_scal_x_int = x_int;
    ppa->srm_scal_rotate.srm_scal_x_frag = x_frag;
    ppa->srm_scal_rotate.srm_scal_y_int = y_int;
    ppa->srm_scal_rotate.srm_scal_y_frag = y_frag;
    ppa->srm_scal_rotate.srm_rotate_angle = angle;
    ppa->srm_scal_rotate.srm_mirror_x = mirror_x;
    ppa->srm_scal_rotate.srm_mirror_y = mirror_y;
    ppa->srm_scal_rotate.scal_rotate_start = 1;   /* WT */

    if (!wait_done(fa, 0, 1u /*srm_eof*/, 200000))
        return -1;
    fbaccel_wb_inv(fa, (uint8_t *)fa->fb + (size_t)dy * fa->stride,
                   (size_t)oh * fa->stride);
    return 0;
}

/* ---------- blend (TX1 BG + TX2 FG + RX2 out) ---------- */

int fbaccel_blend(fbaccel_t *fa,
                  uint32_t bg_x, uint32_t bg_y,
                  uint32_t fg_x, uint32_t fg_y,
                  uint32_t dx, uint32_t dy,
                  uint32_t w, uint32_t h,
                  uint32_t alpha_fg, uint32_t alpha_bg)
{
    volatile dma2d_dev_t *dma = fa->dma2d;
    volatile ppa_dev_t *ppa = fa->ppa;
    fdesc_t *dbg = (fdesc_t *)((uint8_t *)fa->desc + OFF_TX);
    fdesc_t *dfg = (fdesc_t *)((uint8_t *)fa->desc + OFF_FG);
    fdesc_t *drx = (fdesc_t *)((uint8_t *)fa->desc + OFF_RX);
    uint32_t pbg = fa->desc_phys + OFF_TX;
    uint32_t pfg = fa->desc_phys + OFF_FG;
    uint32_t prx = fa->desc_phys + OFF_RX;

    desc_set_2d(dbg, w, h, fa->width, fa->height, bg_x, bg_y, 3,
                (void *)fa->fb_phys);
    desc_set_2d(dfg, w, h, fa->width, fa->height, fg_x, fg_y, 3,
                (void *)fa->fb_phys);
    desc_set_2d(drx, w, h, fa->width, fa->height, dx, dy, 3,
                (void *)fa->fb_phys);

    dma->out_channel[1].out_conf0.out_rst_chn = 1;
    dma->out_channel[1].out_conf0.out_rst_chn = 0;
    dma->out_channel[1].out_peri_sel.out_peri_sel_chn = PERI_PPA_BLEND_BG_TX;
    dma->out_channel[1].out_conf0.out_dscr_port_en_chn = 0;
    dma->out_channel[1].out_conf0.out_reorder_en_chn = 0;
    dma->out_channel[1].out_conf0.outdscr_burst_en_chn = 1;
    dma->out_channel[1].out_conf0.out_mem_burst_length_chn = 3;
    dma->out_channel[1].out_conf0.out_macro_block_size_chn = 3;
    dma->out_channel[1].out_conf0.out_page_bound_en_chn = 1;

    dma->out_channel[2].out_conf0.out_rst_chn = 1;
    dma->out_channel[2].out_conf0.out_rst_chn = 0;
    dma->out_channel[2].out_peri_sel.out_peri_sel_chn = PERI_PPA_BLEND_FG_TX;
    dma->out_channel[2].out_conf0.out_dscr_port_en_chn = 0;
    dma->out_channel[2].out_conf0.outdscr_burst_en_chn = 1;
    dma->out_channel[2].out_conf0.out_mem_burst_length_chn = 3;
    dma->out_channel[2].out_conf0.out_macro_block_size_chn = 3;
    dma->out_channel[2].out_conf0.out_page_bound_en_chn = 1;

    dma->in_channel[2].in_conf0.in_rst_chn = 1;
    dma->in_channel[2].in_conf0.in_rst_chn = 0;
    dma->in_channel[2].in_peri_sel.in_peri_sel_chn = PERI_PPA_BLEND_RX;
    dma->in_channel[2].in_conf0.in_dscr_port_en_chn = 0;
    dma->in_channel[2].in_conf0.in_reorder_en_chn = 0;
    dma->in_channel[2].in_conf0.in_mem_trans_en_chn = 0;
    dma->in_channel[2].in_conf0.indscr_burst_en_chn = 1;
    dma->in_channel[2].in_conf0.in_mem_burst_length_chn = 3;
    dma->in_channel[2].in_conf0.in_macro_block_size_chn = 3;
    dma->in_channel[2].in_conf0.in_page_bound_en_chn = 1;

    /* v36 order: blend core reset + full config BEFORE the channel
     * starts; a reset pulse after arming wipes the handshake. */
    ppa->blend_trans_mode.blend_rst = 1;
    ppa->blend_trans_mode.blend_rst = 0;
    W32(&ppa->int_clr, 0xFFFFFFFFu);
    ppa->blend_color_mode.blend0_rx_cm = 2;
    ppa->blend_color_mode.blend1_rx_cm = 2;
    ppa->blend_color_mode.blend_tx_cm = 2;
    /* one 32-bit store — RMW writes phantom 1s back into reserved bits */
    ppa->blend_fix_alpha.val = (1u << 16) | (1u << 18) |
                               ((alpha_bg & 0xff) << 0) |
                               ((alpha_fg & 0xff) << 8);
    ppa->blend_tx_size.blend_hb = w;
    ppa->blend_tx_size.blend_vb = h;

    fbaccel_wb(fa, (uint8_t *)fa->fb + (size_t)bg_y * fa->stride,
               (size_t)h * fa->stride);
    fbaccel_wb(fa, (uint8_t *)fa->fb + (size_t)fg_y * fa->stride,
               (size_t)h * fa->stride);

    dma->out_channel[1].out_link_addr.outlink_addr_chn = pbg;
    dma->out_channel[2].out_link_addr.outlink_addr_chn = pfg;
    dma->in_channel[2].in_link_addr.inlink_addr_chn = prx;
    dma->out_channel[1].out_link_conf.outlink_start_chn = 1;
    dma->out_channel[2].out_link_conf.outlink_start_chn = 1;
    dma->in_channel[2].in_link_conf.inlink_start_chn = 1;

    ppa->blend_trans_mode.blend_bypass = 0;
    ppa->blend_trans_mode.blend_fix_pixel_fill_en = 0;
    ppa->blend_trans_mode.blend_en = 1;
    ppa->blend_trans_mode.blend_trans_mode_update = 1;

    if (!wait_done(fa, 2, 2u /*blend_eof*/, 200000))
        return -1;
    fbaccel_wb_inv(fa, (uint8_t *)fa->fb + (size_t)dy * fa->stride,
                   (size_t)h * fa->stride);
    return 0;
}
