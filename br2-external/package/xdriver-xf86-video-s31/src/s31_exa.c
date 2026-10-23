/*
 * s31_exa.c — EXA hooks for the ESP32-S31 PPA/DMA2D engines.
 *
 * Solid (rectangle fill)  -> PPA fill    (~213 MB/s)
 * Copy  (screen-to-screen) -> DMA2D M2M  (~106 MB/s)
 *
 * Composite is deliberately NOT hooked (analyzed 2026-10-23): the PPA
 * blend engine merges two fb windows with a FIXED per-plane alpha, but
 * the Render ops a compositor-less IceWM desktop actually issues are
 * antialiased glyph draws — solid color modulated by a per-pixel a8
 * mask — which fixed-alpha hardware cannot express (RGB565 pixmaps
 * carry no alpha, and the engines only reach the fb physical range, so
 * a8 mask pixmaps in RAM are out of bounds too).  The only mappable
 * case, uniform-alpha window-over-window, never occurs without a
 * compositor.  Hooking PrepareComposite would buy diagnostics, not
 * frames.
 *
 * Channel note: JPEG decode (jpegdec) also uses DMA2D ch0 (TX0/RX0),
 * same pair as this driver's Copy path — running jpeg tools while X is
 * mid-blit can glitch one operation.  User-initiated and transient;
 * revisit if a JPEG viewer becomes a permanent X client.
 *
 * All fbaccel operations are synchronous (they spin on the engine EOF
 * interrupt flags), so the EXA sync markers are trivial.
 *
 * Overlap rule for Copy: the DMA walks every region top to bottom, left
 * to right, reading ahead of the writes.  That is only safe when the
 * destination is ABOVE the source (scroll-up) or rows do not overlap.
 * EXA's dxdir/dydir are mi's suggested copy order and do not map to the
 * actual hazard (a pure vertical scroll arrives as dxdir=-1), so
 * PrepareCopy accepts everything well-formed and s31_copy decides from
 * the real per-box geometry: unsafe (destination below source, rows
 * overlapping) goes to a CPU fallback that copies bottom-to-top with
 * memmove, which is correct for every overlap.
 */

#include <stdint.h>
#include <string.h>

#include <xorg-server.h>
#include "xf86.h"
#include "exa.h"

#include "fbaccel.h"

typedef struct {
    fbaccel_t   fa;
    CARD8      *fbmem;      /* Xorg's own fb mapping (fbdevHW VA) */
    Bool        up;
    uint32_t    marker;
    /* state collected between Prepare* and the op callbacks */
    Pixel       solid_fg;
    intptr_t    src_off;    /* src pixmap offset in the fb, bytes */
    intptr_t    dst_off;
} S31ExaRec;

static S31ExaRec g_exa;
static uint32_t g_stride;   /* fb line length in bytes */

/* diagnostics: why ops miss the engines (dumped to the X log every 128) */
static uint32_t g_n_solid, g_n_solid_prep_rej, g_n_solid_eng_fail;
static uint32_t g_n_copy, g_n_copy_prep_rej, g_n_copy_eng_fail, g_n_copy_cpu;

static void
s31_diag_tick(void)
{
    if (((g_n_solid + g_n_copy) & 127) == 0)
        ErrorF("S31EXA diag: solid=%u(prej=%u,engfail=%u) "
               "copy=%u(prej=%u,engfail=%u,cpu_geom=%u)\n",
               g_n_solid, g_n_solid_prep_rej, g_n_solid_eng_fail,
               g_n_copy, g_n_copy_prep_rej, g_n_copy_eng_fail, g_n_copy_cpu);
}

/* Byte offset of a pixmap's origin inside the framebuffer.
 * MUST go through exaGetPixmapOffset: in classic EXA the drawable's
 * devPrivate.ptr is not maintained for GPU-resident pixmaps (observed
 * NULL at PrepareSolid time); only the EXA pixmap private's fb_ptr is
 * valid.  exaGetPixmapOffset() = fb_ptr - memoryBase. */
static intptr_t
s31_pix_off(const S31ExaRec *ex, PixmapPtr pPix)
{
    (void)ex;
    return (intptr_t)exaGetPixmapOffset(pPix);
}

/* A pixmap offset must name memory inside the fb before the engines may
 * touch it; anything else means EXA handed us a pixmap we do not
 * understand -- refuse, so a bad pointer can never become a wild DMA. */
static Bool
s31_off_ok(intptr_t off)
{
    return off >= 0 && (uint64_t)off < (uint64_t)g_exa.fa.fb_len;
}

static void
s31_off_to_xy(intptr_t off, uint32_t *x, uint32_t *y)
{
    *y = (uint32_t)(off / g_stride);
    *x = (uint32_t)((off % g_stride) / 2);   /* 2 bytes per RGB565 pixel */
}

/* ------------------------------------------------------------------ */
/* sync                                                                */

static int
s31_mark_sync(ScreenPtr pScreen)
{
    return (int)++g_exa.marker;
}

static void
s31_wait_marker(ScreenPtr pScreen, int marker)
{
    /* every engine op has already waited for completion */
    (void)pScreen;
    (void)marker;
}

/* ------------------------------------------------------------------ */
/* CPU access to pixmaps: the fb is always mapped, nothing to prepare  */

static Bool
s31_prepare_access(PixmapPtr pPix, int index)
{
    (void)pPix;
    (void)index;
    return TRUE;
}

static void
s31_finish_access(PixmapPtr pPix, int index)
{
    (void)pPix;
    (void)index;
}

/* ------------------------------------------------------------------ */
/* Solid                                                               */

static Bool
s31_prepare_solid(PixmapPtr pPix, int alu, Pixel planemask, Pixel fg)
{
    if (alu != GXcopy || (planemask & 0xffff) != 0xffff || !g_exa.up) {
        g_n_solid_prep_rej++;
        s31_diag_tick();
        return FALSE;
    }
    {
        static int n_solid_entry;
        if (n_solid_entry < 3) {
            n_solid_entry++;
            ErrorF("S31EXA prepare_solid entry #%d\n", n_solid_entry);
        }
    }
    g_n_solid++;
    g_exa.solid_fg = fg;
    g_exa.dst_off = s31_pix_off(&g_exa, pPix);
    if (!s31_off_ok(g_exa.dst_off)) {
        if (g_n_solid_prep_rej == 0)
            ErrorF("S31EXA offrange solid: off=%ld pitch=%lu\n",
                   (long)g_exa.dst_off, exaGetPixmapPitch(pPix));
        g_n_solid_prep_rej++;
        s31_diag_tick();
        return FALSE;
    }
    /* the engines walk lines of g_stride; foreign pitches go to the CPU */
    if (exaGetPixmapPitch(pPix) != g_stride) {
        g_n_solid_prep_rej++;
        s31_diag_tick();
        return FALSE;
    }
    return TRUE;
}

static void
s31_solid(PixmapPtr pPix, int x1, int y1, int x2, int y2)
{
    uint32_t px, py;
    int w = x2 - x1, h = y2 - y1;

    if (w <= 0 || h <= 0)
        return;
    s31_off_to_xy(g_exa.dst_off, &px, &py);
    px += x1; py += y1;
    /* never let a bad rect become a wild DMA: clamp to the fb */
    if (px >= g_exa.fa.width || py >= g_exa.fa.height)
        return;
    if (px + (uint32_t)w > g_exa.fa.width)
        w = (int)(g_exa.fa.width - px);
    if (py + (uint32_t)h > g_exa.fa.height)
        h = (int)(g_exa.fa.height - py);
    if (fbaccel_fill(&g_exa.fa, px, py, (uint32_t)w, (uint32_t)h,
                     (uint16_t)(g_exa.solid_fg & 0xffff)) != 0) {
        /* engine failed: finish the rectangle on the CPU */
        uint16_t *base = (uint16_t *)((char *)g_exa.fa.fb + g_exa.dst_off);
        uint32_t stride_px = g_stride / 2;
        g_n_solid_eng_fail++;
        s31_diag_tick();
        for (int yy = y1; yy < y2; yy++)
            for (int xx = x1; xx < x2; xx++)
                base[yy * stride_px + xx] = (uint16_t)(g_exa.solid_fg & 0xffff);
    }
}

static void
s31_done_solid(PixmapPtr pPix)
{
    (void)pPix;
}

/* ------------------------------------------------------------------ */
/* Copy                                                                */

static Bool
s31_prepare_copy(PixmapPtr pSrc, PixmapPtr pDst, int dxdir, int dydir,
                 int alu, Pixel planemask)
{
    static int n_entry;
    if (n_entry < 3) {
        n_entry++;
        ErrorF("S31EXA prepare_copy entry #%d: dxdir=%d dydir=%d alu=%d\n",
               n_entry, dxdir, dydir, alu);
    }
    if (alu != GXcopy || (planemask & 0xffff) != 0xffff || !g_exa.up) {
        g_n_copy_prep_rej++;
        s31_diag_tick();
        return FALSE;
    }
    /* dxdir/dydir are mi's suggested traversal order and do not map to
     * the real hazard (a pure vertical scroll arrives as dxdir=-1);
     * s31_copy decides from the actual per-box geometry instead */
    (void)dxdir;
    (void)dydir;
    g_n_copy++;
    g_exa.src_off = s31_pix_off(&g_exa, pSrc);
    g_exa.dst_off = s31_pix_off(&g_exa, pDst);
    if (!s31_off_ok(g_exa.src_off) || !s31_off_ok(g_exa.dst_off) ||
        exaGetPixmapPitch(pSrc) != g_stride ||
        exaGetPixmapPitch(pDst) != g_stride) {
        g_n_copy_prep_rej++;
        s31_diag_tick();
        return FALSE;
    }
    return TRUE;
}

static void
s31_copy(PixmapPtr pDst, int srcX, int srcY, int dstX, int dstY,
         int w, int h)
{
    uint32_t sx, sy, dx, dy;

    if (w <= 0 || h <= 0)
        return;
    s31_off_to_xy(g_exa.src_off, &sx, &sy);
    s31_off_to_xy(g_exa.dst_off, &dx, &dy);
    sx += srcX; sy += srcY; dx += dstX; dy += dstY;
    /* clamp both ends to the fb: a bad rect must never reach the DMA */
    if (sx >= g_exa.fa.width || sy >= g_exa.fa.height ||
        dx >= g_exa.fa.width || dy >= g_exa.fa.height)
        return;
    if (sx + (uint32_t)w > g_exa.fa.width) w = (int)(g_exa.fa.width - sx);
    if (dx + (uint32_t)w > g_exa.fa.width) w = (int)(g_exa.fa.width - dx);
    if (sy + (uint32_t)h > g_exa.fa.height) h = (int)(g_exa.fa.height - sy);
    if (dy + (uint32_t)h > g_exa.fa.height) h = (int)(g_exa.fa.height - dy);
    if (w <= 0 || h <= 0)
        return;

    /* The DMA reads ahead while writing top-to-bottom: with rows
     * overlapping and the destination BELOW the source it would write
     * pixels the TX side has not read yet.  The CPU fallback copies
     * bottom-to-top with memmove, which is correct for every overlap
     * (memmove handles the within-line hazard, reverse line order the
     * cross-line one). */
    Bool rows_overlap = dy < sy + (uint32_t)h && sy < dy + (uint32_t)h;
    Bool engine_safe = !rows_overlap || dy <= sy;

    if (engine_safe &&
        fbaccel_blit(&g_exa.fa, dx, dy, sx, sy,
                     (uint32_t)w, (uint32_t)h) == 0)
        return;

    if (!engine_safe)
        g_n_copy_cpu++;         /* geometry fallback, not an engine fault */
    else {
        g_n_copy_eng_fail++;    /* engine accepted but failed */
        s31_diag_tick();
    }
    uint16_t *src = (uint16_t *)((char *)g_exa.fa.fb + g_exa.src_off);
    uint16_t *dst = (uint16_t *)((char *)g_exa.fa.fb + g_exa.dst_off);
    uint32_t stride_px = g_stride / 2;
    if (dy > sy) {
        for (int yy = h - 1; yy >= 0; yy--)
            memmove(&dst[(dstY + yy) * stride_px + dstX],
                    &src[(srcY + yy) * stride_px + srcX], (size_t)w * 2);
    } else {
        for (int yy = 0; yy < h; yy++)
            memmove(&dst[(dstY + yy) * stride_px + dstX],
                    &src[(srcY + yy) * stride_px + srcX], (size_t)w * 2);
    }
}

static void
s31_done_copy(PixmapPtr pDst)
{
    (void)pDst;
}

/* ------------------------------------------------------------------ */
/* init                                                                */

Bool
S31ExaInit(ScrnInfoPtr pScrn, ScreenPtr pScreen, CARD8 *fbmem)
{
    ExaDriverPtr exa;

    memset(&g_exa, 0, sizeof(g_exa));
    if (fbaccel_init(&g_exa.fa) != 0) {
        xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
                   "s31: fbaccel_init failed, no PPA/DMA2D acceleration\n");
        return FALSE;
    }
    g_exa.up = TRUE;
    g_exa.fbmem = fbmem;
    g_stride = g_exa.fa.stride;

    exa = exaDriverAlloc();
    if (!exa) {
        fbaccel_close(&g_exa.fa);
        g_exa.up = FALSE;
        return FALSE;
    }

    exa->exa_major = EXA_VERSION_MAJOR;
    exa->exa_minor = EXA_VERSION_MINOR;

    /*
     * EXA in this server (21.1) requires EXA_OFFSCREEN_PIXMAPS: without it
     * the pixmap private key is never registered and the first operation
     * aborts in dixGetPrivateAddr.  The fb holds exactly one screen
     * (768000 of 786432 bytes), so the offscreen heap is 0 bytes and all
     * non-screen pixmaps stay in system RAM (CPU-rendered, correct).
     *
     * memoryBase MUST be Xorg's own fb mapping (fbdevHW VA): classic
     * EXA's ModifyPixmapHeader marks the screen pixmap as having a GPU
     * copy only if its pointer is inside [memoryBase, memoryBase+size).
     * Passing fbaccel's separate mmap of the same physical fb fails that
     * check and everything silently CPU-renders (measured 64 ms fills).
     */
    exa->memoryBase = fbmem;
    exa->memorySize = g_exa.fa.fb_len;
    exa->offScreenBase = g_exa.fa.fb_len;   /* 0-byte heap */
    exa->flags = EXA_OFFSCREEN_PIXMAPS;
    exa->pixmapPitchAlign = 64;             /* DMA burst granularity */
    exa->pixmapOffsetAlign = 64;
    exa->maxX = 16383;      /* PPA descriptor fields are 14 bit */
    exa->maxY = 16383;

    exa->MarkSync = s31_mark_sync;
    exa->WaitMarker = s31_wait_marker;
    exa->PrepareAccess = s31_prepare_access;
    exa->FinishAccess = s31_finish_access;

    exa->PrepareSolid = s31_prepare_solid;
    exa->Solid = s31_solid;
    exa->DoneSolid = s31_done_solid;

    exa->PrepareCopy = s31_prepare_copy;
    exa->Copy = s31_copy;
    exa->DoneCopy = s31_done_copy;

    if (!exaDriverInit(pScreen, exa)) {
        fbaccel_close(&g_exa.fa);
        g_exa.up = FALSE;
        return FALSE;
    }

    xf86DrvMsg(pScrn->scrnIndex, X_INFO,
               "s31: EXA on PPA/DMA2D (fill + blit), fb=%u bytes stride=%u\n",
               (unsigned)g_exa.fa.fb_len, (unsigned)g_stride);
    return TRUE;
}
