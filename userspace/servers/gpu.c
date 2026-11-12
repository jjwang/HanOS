/**-----------------------------------------------------------------------------

 @file    gpu.c
 @brief   Userspace Intel display driver and desktop compositor

 @details
 @verbatim

   The kernel grants the GTTMMADR and GMADR BARs plus the boot timing. This
   server sets the eDP mode, owns the GTT allocations, composes the rain
   desktop and hardware cursor, and hands its console surface to the console
   server through a memory object.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <bootinfo.h>
#include <protocol.h>
#include <sysfunc.h>

#include <gpu/gfx.h>
#include <gpu_reg.h>
#include <gpu/gpu_log.h>
#include <gpu/gpu_cursor.h>

#define GPU_GTT_BASE        0x05000000UL
#define GPU_FENCE_BASE      0x00100000UL
#define GPU_FENCE_COUNT     16
#define GPU_HEAP_VADDR      0x60000000UL
#define GPU_HEAP_LIMIT      0x80000000UL

#define LAYER_UI_W          480
#define LAYER_UI_H          180
#define LAYER_UI_X          80
#define LAYER_UI_Y          80
#define LAYER_SCROLL_SPEED  8
#define LAYER_PERIOD_SCREENS 4

typedef struct {
    uint8_t magic[2];
    uint8_t mode;
    uint8_t charsize;
    uint8_t data[];
} psf1_t;

extern psf1_t term_font_norm;

static gfx_pci_t gpu_pci;
static gfx_gtt_t gpu_gtt;
static gfx_mem_manager_t gpu_mgr;
static gfx_fb_t gpu_primary;
static uint64_t gpu_heap_next = GPU_HEAP_VADDR;

static bool desktop_active;
static gfx_fb_t desktop_tex;
static gfx_fb_t desktop_sprite;
static uint32_t desktop_period;
static uint32_t desktop_scroll;
static uint32_t desktop_last_frame;
static uint32_t layer_rng = 0x9e3779b9u;

void gpu_log(const char *fmt, ...)
{
    char buf[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    sys_libc_log(buf);
}

void pit_wait(uint32_t ms)
{
    usleep(ms * 1000);
}

void gpu_flush(const void *addr, uint64_t len)
{
    uintptr_t p = (uintptr_t) addr & ~63ULL;
    uintptr_t end = (uintptr_t) addr + len;

    for (; p < end; p += 64)
        __asm__ volatile ("clflush (%0)"::"r"((void *)p):"memory");
    __asm__ volatile ("mfence":::"memory");
}

void gfx_gtt_write_entry(gfx_gtt_t *gtt, uint32_t index, uint64_t phys)
{
    if (gtt->pte64) {
        uint64_t entry = phys & ~(uint64_t)(GPU_PAGE_SIZE - 1);

        entry |= GTT_ENTRY_VALID;
        gtt->entries[index] = entry;
    } else {
        uint32_t entry = (uint32_t)(phys & 0xffffffffu)
            & ~(uint32_t)(GPU_PAGE_SIZE - 1);

        entry |= GTT_ENTRY_VALID;
        ((volatile uint32_t *)gtt->entries)[index] = entry;
    }
}

uint32_t gfx_gtt_map(gfx_gtt_t *gtt, uint64_t gpu_addr, uint64_t phys,
                     uint64_t size)
{
    uint32_t start = (uint32_t)(gpu_addr >> GPU_PAGE_SHIFT);
    uint32_t pages = (uint32_t)GPU_NUM_PAGES(size);

    if (start + pages > gtt->num_total_entries)
        return 0;

    for (uint32_t i = 0; i < pages; i++)
        gfx_gtt_write_entry(gtt, start + i, phys + (uint64_t)i * GPU_PAGE_SIZE);

    (void)gtt->entries[start];
    return pages;
}

static bool gpu_alloc_pages(uint64_t size, uint64_t align, uint64_t *out_vaddr,
                            int64_t *out_handle, uint64_t *out_first_phys,
                            uint32_t *out_pages)
{
    size = PAGE_ALIGN_UP(size);
    uint64_t vaddr = gpu_heap_next;

    if (align > GPU_PAGE_SIZE)
        vaddr = (vaddr + align - 1) & ~(align - 1);
    if (vaddr + size > GPU_HEAP_LIMIT)
        return false;

    int64_t memh = sys_mem_alloc(size);

    if (memh < 0)
        return false;
    if (sys_mem_map(memh, vaddr, 3) != 0)
        return false;

    uint64_t first = (uint64_t)sys_mem_phys(memh, 0);

    if (first == (uint64_t)-1)
        return false;

    gpu_heap_next = vaddr + size;
    *out_vaddr = vaddr;
    *out_handle = memh;
    *out_first_phys = first;
    *out_pages = (uint32_t)(size / GPU_PAGE_SIZE);
    return true;
}

bool gfx_alloc(gfx_mem_manager_t *mgr, gfx_gtt_t *gtt, gfx_object_t *obj,
               uint64_t size, uint64_t align)
{
    uint64_t gpu_addr = mgr->shared.current;
    uint64_t vaddr, first;
    int64_t memh;
    uint32_t pages;

    if (align > GPU_PAGE_SIZE)
        gpu_addr = (gpu_addr + align - 1) & ~(align - 1);
    if (gpu_addr + PAGE_ALIGN_UP(size) > mgr->shared.top)
        return false;
    if (!gpu_alloc_pages(size, align, &vaddr, &memh, &first, &pages))
        return false;

    for (uint32_t i = 0; i < pages; i++) {
        uint64_t phys = (uint64_t)sys_mem_phys(memh, (uint64_t)i * GPU_PAGE_SIZE);

        if (phys == (uint64_t)-1)
            return false;
        gfx_gtt_write_entry(gtt, (uint32_t)((gpu_addr >> GPU_PAGE_SHIFT) + i),
                            phys);
    }
    (void)gtt->entries[gpu_addr >> GPU_PAGE_SHIFT];

    memset(obj, 0, sizeof(*obj));
    obj->cpu_addr = (volatile uint8_t *)vaddr;
    obj->gfx_addr = gpu_addr;
    obj->phys = first;
    obj->size = PAGE_ALIGN_UP(size);
    obj->pages = pages;
    obj->mem_handle = memh;
    mgr->shared.current = gpu_addr + PAGE_ALIGN_UP(size);
    return true;
}

static bool gpu_fb_alloc(uint32_t width, uint32_t height, gfx_fb_t *out)
{
    uint64_t stride = (width * 4 + 63) & ~63ULL;

    memset(out, 0, sizeof(*out));
    if (!gfx_alloc(&gpu_mgr, &gpu_gtt, &out->obj, stride * height, 64))
        return false;
    out->width = width;
    out->height = height;
    out->stride = (uint32_t)stride;
    out->format = DISPPLANE_BGRX888;
    out->mem_handle = out->obj.mem_handle;
    memset((void *)out->obj.cpu_addr, 0, (uint64_t)out->stride * out->height);
    gpu_flush((const void *)out->obj.cpu_addr, (uint64_t)out->stride * out->height);
    return true;
}

bool gfx_fb_alloc(uint32_t width, uint32_t height, gfx_fb_t *out_fb)
{
    return gpu_fb_alloc(width, height, out_fb);
}

void gfx_fb_free(gfx_fb_t *fb)
{
    (void)fb;
}

uint32_t gfx_pipe_frame(void)
{
    return gfx_ind(&gpu_pci, 0x70040);
}

bool gfx_wait_vblank_frame(void)
{
    uint32_t start = gfx_pipe_frame();

    for (int i = 0; i < 100; i++) {
        if (gfx_pipe_frame() != start)
            return true;
        usleep(500);
    }
    return false;
}

static bool gpu_init(const bootinfo_t *bi)
{
    memset(&gpu_pci, 0, sizeof(gpu_pci));
    memset(&gpu_gtt, 0, sizeof(gpu_gtt));
    memset(&gpu_mgr, 0, sizeof(gpu_mgr));

    gpu_pci.device_id = (uint32_t)bi->gpu_device_id;
    gpu_pci.gen9 = bi->gpu_gen9 != 0;
    gpu_pci.pte64 = bi->gpu_pte64 != 0;
    gpu_pci.mmio_bar = (volatile void *)bi->gpu_mmio_vaddr;
    gpu_pci.gtt_offset = (uint32_t)bi->gpu_gtt_offset;
    gpu_pci.gtt_size = (uint32_t)bi->gpu_gtt_size;
    gpu_pci.gtt_addr = (volatile uint64_t *)
        ((uint8_t *)gpu_pci.mmio_bar + gpu_pci.gtt_offset);
    gpu_pci.aperture_bar = (volatile void *)bi->gpu_aperture_vaddr;
    gpu_pci.aperture_size = (uint32_t)bi->gpu_aperture_size;

    gpu_gtt.gtt_mem_size = gpu_pci.gtt_size;
    gpu_gtt.pte64 = gpu_pci.pte64;
    gpu_gtt.num_total_entries = gpu_gtt.gtt_mem_size
        / (gpu_gtt.pte64 ? sizeof(uint64_t) : sizeof(uint32_t));
    gpu_gtt.num_mappable_entries = gpu_pci.aperture_size >> GPU_PAGE_SHIFT;
    gpu_gtt.entries = gpu_pci.gtt_addr;

    gpu_mgr.shared.base = GPU_GTT_BASE;
    if (bi->gpu_stolen_size > gpu_mgr.shared.base)
        gpu_mgr.shared.base = bi->gpu_stolen_size;
    gpu_mgr.shared.current = gpu_mgr.shared.base;
    gpu_mgr.shared.top = (uint64_t)gpu_gtt.num_mappable_entries << GPU_PAGE_SHIFT;
    if (gpu_mgr.shared.top > GPU_HEAP_VADDR)
        gpu_mgr.shared.top = GPU_HEAP_VADDR;
    gpu_mgr.priv.base = gpu_mgr.shared.top;
    gpu_mgr.priv.current = gpu_mgr.priv.base;
    gpu_mgr.priv.top = (uint64_t)gpu_gtt.num_total_entries << GPU_PAGE_SHIFT;
    gpu_mgr.gfx_mem_base = gpu_pci.aperture_bar;

    return true;
}

static uint32_t layer_rand(void)
{
    layer_rng = layer_rng * 1103515245u + 12345u;
    return layer_rng >> 8;
}

static uint32_t layer_hash(uint32_t seed, uint32_t row)
{
    uint32_t x = seed + row * 0x9e3779b9u;

    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    return x;
}

static void layer_pixel(gfx_fb_t *fb, int32_t x, int32_t y, uint32_t color)
{
    if (x < 0 || y < 0 || (uint32_t)x >= fb->width
        || (uint32_t)y >= fb->height)
        return;
    *(uint32_t *)(fb->obj.cpu_addr + (uint64_t)y * fb->stride
                  + (uint64_t)x * 4) = color;
}

static uint32_t layer_get(gfx_fb_t *fb, int32_t x, int32_t y)
{
    return *(uint32_t *)(fb->obj.cpu_addr + (uint64_t)y * fb->stride
                         + (uint64_t)x * 4);
}

static void layer_glyph(gfx_fb_t *fb, int32_t x, int32_t y, uint8_t ch,
                        uint32_t color)
{
    const psf1_t *font = &term_font_norm;
    const uint8_t *glyph = font->data + (uint32_t)ch * font->charsize;

    for (uint32_t i = 0; i < font->charsize; i++) {
        uint8_t bits = glyph[i];

        for (uint32_t k = 0; k < 8; k++)
            if (bits & (0x80u >> k))
                layer_pixel(fb, x + (int32_t)k, y + (int32_t)i, color);
    }
}

static uint32_t layer_rain_color(int32_t d, int32_t len)
{
    uint32_t lvl;

    if (d <= 0)
        return 0xddffdd;
    lvl = (uint32_t)(len - d) * 255u / (uint32_t)len;
    if (lvl > 150)
        return 0x55ff88;
    if (lvl > 90)
        return 0x22cc55;
    if (lvl > 40)
        return 0x00aa33;
    return 0x004411;
}

static void layer_tex_build(gfx_fb_t *tex, uint32_t period_rows,
                            uint32_t screen_rows)
{
    static const char set[] =
        "01010101010101011123456789ABCDEF#$%&*<>[]{}()/=+-:.";
    uint32_t ns = sizeof(set) - 1;
    uint32_t ncol = tex->width / 8;
    uint32_t fh = term_font_norm.charsize;

    memset((void *)tex->obj.cpu_addr, 0, (uint64_t)tex->stride * tex->height);

    for (uint32_t c = 0; c < ncol; c++) {
        uint32_t seed = layer_rand() | 1;
        int32_t pos = (int32_t)(layer_rand() % period_rows);

        while (pos < (int32_t)period_rows) {
            int32_t len = 3 + (int32_t)(layer_rand() % 14);
            int32_t gap = 10 + (int32_t)(layer_rand() % 50);

            for (int32_t i = 0; i < len; i++) {
                int32_t r = (pos + i) % (int32_t)period_rows;
                int32_t d = (len - 1) - i;
                uint8_t ch = (uint8_t)set[layer_hash(seed, (uint32_t)r) % ns];

                layer_glyph(tex, (int32_t)c * 8, r * (int32_t)fh, ch,
                            layer_rain_color(d, len));
            }
            pos += len + gap;
        }
    }

    for (uint32_t r = 0; r < screen_rows; r++)
        for (uint32_t y = 0; y < fh; y++)
            for (uint32_t x = 0; x < tex->width; x++)
                layer_pixel(tex, (int32_t)x,
                            (int32_t)((period_rows + r) * fh + y),
                            layer_get(tex, (int32_t)x,
                                      (int32_t)(r * fh + y)));

    gpu_flush((const void *)tex->obj.cpu_addr,
              (uint64_t)tex->stride * tex->height);
}

static uint32_t desktop_width;
static uint32_t desktop_height;
static uint32_t desktop_period_rows;
static uint32_t desktop_spr_w;
static uint32_t desktop_spr_h;
static uint32_t desktop_pos_x;
static uint32_t desktop_pos_y;
static bool desktop_prepared;

/* Allocate and bake every surface before the mode set touches the pipe. The
 * firmware frame stays visible while the 21 MB texture is built instead of
 * leaving a blank native frame on screen. */
static bool gpu_desktop_prepare(uint32_t width, uint32_t height)
{
    uint32_t screen_rows;
    uint32_t tex_h;
    uint32_t cols;
    uint32_t rows;

    if (desktop_prepared)
        return true;
    if (!gpu_pci.gen9 || width == 0 || height == 0)
        return false;

    screen_rows = (height + term_font_norm.charsize - 1)
        / term_font_norm.charsize;
    desktop_period_rows = LAYER_PERIOD_SCREENS * screen_rows;
    tex_h = (desktop_period_rows + screen_rows) * term_font_norm.charsize;

    if (!gpu_fb_alloc(width, tex_h, &desktop_tex)) {
        kloge("gpu: rain texture allocation failed\n");
        return false;
    }
    layer_tex_build(&desktop_tex, desktop_period_rows, screen_rows);

    cols = (width / 8) * 4 / 5;
    rows = (height / 15) * 4 / 5;
    desktop_spr_w = cols * 8;
    desktop_spr_h = rows * 15 + 20;
    if (!gpu_fb_alloc(desktop_spr_w, desktop_spr_h, &desktop_sprite)) {
        kloge("gpu: console sprite allocation failed\n");
        return false;
    }
    gpu_flush((const void *)desktop_sprite.obj.cpu_addr,
              (uint64_t)desktop_sprite.stride * desktop_sprite.height);

    desktop_width = width;
    desktop_height = height;
    desktop_pos_x = (width - desktop_spr_w) / 2;
    desktop_pos_y = (height - desktop_spr_h) / 2;
    desktop_prepared = true;
    return true;
}

static bool gpu_desktop_commit(void)
{
    uint32_t ctl;

    if (desktop_active)
        return true;
    if (!desktop_prepared
        || gpu_primary.width != desktop_width
        || gpu_primary.height != desktop_height)
        return false;

    gfx_outd(&gpu_pci, PLANE_BUF_CFG_1A,
             PLANE_BUF_END(445) | PLANE_BUF_START(0));
    gfx_outd(&gpu_pci, CUR_BUF_CFG,
             PLANE_BUF_END(477) | PLANE_BUF_START(446));
    gfx_outd(&gpu_pci, PLANE_BUF_CFG_2A,
             PLANE_BUF_END(511) | PLANE_BUF_START(478));

    ctl = gfx_ind(&gpu_pci, PLANE_CTL_1_A);
    gfx_outd(&gpu_pci, PLANE_CTL_1_A, 0);
    gfx_wait_vblank_frame();
    gfx_outd(&gpu_pci, PLANE_POS_1_A, 0);
    gfx_outd(&gpu_pci, PLANE_SIZE_1_A,
             (desktop_height - 1) << 16 | (desktop_width - 1));
    gfx_outd(&gpu_pci, PLANE_STRIDE_1_A, desktop_tex.stride / 64);
    gfx_outd(&gpu_pci, PLANE_OFFSET_1_A,
             desktop_period_rows * term_font_norm.charsize << 16);
    gfx_outd(&gpu_pci, PLANE_CTL_1_A, ctl);
    gfx_outd(&gpu_pci, PLANE_SURF_1_A, (uint32_t)desktop_tex.obj.gfx_addr);
    gfx_wait_vblank_frame();

    gfx_outd(&gpu_pci, PLANE_CTL_2_A, 0);
    gfx_outd(&gpu_pci, PS_CTRL_2A, 0);
    gfx_outd(&gpu_pci, PLANE_OFFSET_2_A, 0);
    gfx_outd(&gpu_pci, PLANE_POS_2_A,
             (desktop_pos_y << 16) | desktop_pos_x);
    gfx_outd(&gpu_pci, PLANE_SIZE_2_A,
             (desktop_sprite.height - 1) << 16
             | (desktop_sprite.width - 1));
    gfx_outd(&gpu_pci, PLANE_STRIDE_2_A, desktop_sprite.stride / 64);
    for (uint32_t lvl = 0; lvl < 8; lvl++)
        gfx_outd(&gpu_pci, PLANE_WM_2A(lvl),
                 PLANE_WM_EN | PLANE_WM_IGNORE_LINES | PLANE_WM_BLOCKS(16));
    gfx_outd(&gpu_pci, PLANE_WM_TRANS_2A,
             PLANE_WM_EN | PLANE_WM_IGNORE_LINES | PLANE_WM_BLOCKS(16));
    gfx_outd(&gpu_pci, PLANE_CTL_2_A,
             PLANE_CTL_ENABLE | PLANE_CTL_FORMAT_XRGB
             | PLANE_CTL_ALPHA_SW_PREMULTIPLY | PLANE_CTL_TILED_LINEAR);
    gfx_outd(&gpu_pci, PLANE_SURF_2_A,
             (uint32_t)desktop_sprite.obj.gfx_addr);
    (void)gfx_ind(&gpu_pci, PLANE_SURF_2_A);
    gfx_wait_vblank_frame();

    desktop_period = desktop_period_rows * term_font_norm.charsize;
    desktop_scroll = desktop_period;
    desktop_last_frame = gfx_pipe_frame();
    desktop_active = true;

    klogi("gpu: desktop %ux%u period %u, console %ux%u at %u,%u\n",
          desktop_width, desktop_height, desktop_period, desktop_spr_w,
          desktop_spr_h, desktop_pos_x, desktop_pos_y);
    return true;
}

static void gpu_desktop_tick(void)
{
    uint32_t now;

    if (!desktop_active)
        return;

    now = gfx_pipe_frame();
    if (now == desktop_last_frame)
        return;
    desktop_last_frame = now;
    if (desktop_scroll >= LAYER_SCROLL_SPEED)
        desktop_scroll -= LAYER_SCROLL_SPEED;
    else
        desktop_scroll = desktop_period;
    gfx_outd(&gpu_pci, PLANE_OFFSET_1_A, desktop_scroll << 16);
    gfx_outd(&gpu_pci, PLANE_SURF_1_A,
             (uint32_t)desktop_tex.obj.gfx_addr);
}

static void gpu_console_reply(sys_ipc_msg_t *m, sys_ipc_msg_t *rep)
{
    gfx_fb_t *src = desktop_active ? &desktop_sprite : &gpu_primary;

    if (src->mem_handle <= 0) {
        rep->words[0] = (uint64_t)(int64_t)-1;
    } else {
        int64_t dup = sys_handle_dup(src->mem_handle);

        if (dup < 0) {
            rep->words[0] = (uint64_t)(int64_t)-1;
        } else {
            rep->words[0] = 0;
            rep->words[1] = src->width;
            rep->words[2] = src->height;
            rep->words[3] = src->stride;
            rep->words[4] = desktop_active ? 1 : 0;
            rep->xfer[0] = (uint64_t)dup;
            rep->xfer_count = 1;
        }
    }

    if (m->xfer[0] != 0) {
        sys_ipc_send((int64_t)m->xfer[0], rep);
        sys_handle_close((int64_t)m->xfer[0]);
    }
}

static void gpu_serve(sys_ipc_msg_t *m)
{
    sys_ipc_msg_t rep;

    memset(&rep, 0, sizeof(rep));
    rep.tag = m->tag;

    if (m->tag == GPU_CURSOR_MOVE) {
        skl_cursor_move((int32_t)(int64_t)m->words[0],
                        (int32_t)(int64_t)m->words[1]);
        return;
    }
    if (m->tag == GPU_CONSOLE_SURFACE) {
        gpu_console_reply(m, &rep);
        return;
    }
}

int main(void)
{
    bootinfo_t bi;

    while (sys_bootinfo(&bi) < 0 || bi.magic != BOOTINFO_MAGIC)
        ;

    gpu_log("gpu: bootinfo mmio 0x%lx/0x%lx aperture 0x%lx/0x%lx gtt 0x%lx/0x%lx\n",
            (unsigned long)bi.gpu_mmio_vaddr, (unsigned long)bi.gpu_mmio_size,
            (unsigned long)bi.gpu_aperture_vaddr,
            (unsigned long)bi.gpu_aperture_size,
            (unsigned long)bi.gpu_gtt_offset,
            (unsigned long)bi.gpu_gtt_size);

    if (!gpu_init(&bi))
        return 1;

    gpu_log("gpu: init stolen 0x%lx base 0x%lx shared 0x%lx/0x%lx\n",
            (unsigned long)bi.gpu_stolen_size,
            (unsigned long)bi.gpu_stolen_base,
            (unsigned long)gpu_mgr.shared.base,
            (unsigned long)gpu_mgr.shared.top);

    if (!bi.gpu_display_ready) {
        kloge("gpu: kernel display bootstrap did not switch the panel\n");
        return 1;
    }

    uint32_t disp_w = (uint32_t)bi.gpu_boot_fb_width;
    uint32_t disp_h = (uint32_t)bi.gpu_boot_fb_height;
    uint32_t disp_pitch = (uint32_t)bi.gpu_boot_fb_pitch;

    (void)gpu_desktop_prepare(disp_w, disp_h);

    gpu_primary.width = disp_w;
    gpu_primary.height = disp_h;
    gpu_primary.stride = disp_pitch;

    if (skl_cursor_init(&gpu_pci, &gpu_mgr, &gpu_gtt,
                        gpu_primary.width, gpu_primary.height))
        skl_cursor_set((int32_t)gpu_primary.width / 2,
                       (int32_t)gpu_primary.height / 2);

    (void)gpu_desktop_commit();

    for (;;) {
        sys_ipc_msg_t m;

        if (sys_ipc_recv_timeout((int64_t)bi.service_ep, &m, 8) == 0)
            gpu_serve(&m);
        gpu_desktop_tick();
    }
}
