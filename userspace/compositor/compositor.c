#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <wayland-server-core.h>
#include <wayland-server.h>
#include <sysfunc.h>
#include <sys/mman.h>
#include <time.h>
#include <protocol.h>

struct surface {
    struct wl_resource *buffer;
    struct wl_resource *res;
    struct wl_resource *frame_cb;
};

static struct wl_resource *g_keyboard;
static int64_t g_tty_ep = -1, g_notify_ep = -1;
static uint32_t g_key_serial;

static uint32_t now_ms(void);

static int canvas_fd = -1;
static uint32_t *canvas;
static int canvas_w, canvas_h;

static void s_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void s_attach(struct wl_client *c, struct wl_resource *r, struct wl_resource *buf, int32_t x, int32_t y)
{ (void)c;(void)x;(void)y; struct surface *s = wl_resource_get_user_data(r); s->buffer = buf; }
static void s_damage(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h)
{ (void)c;(void)r;(void)x;(void)y;(void)w;(void)h; }
static void s_frame(struct wl_client *c, struct wl_resource *r, uint32_t cb)
{
    struct surface *s = wl_resource_get_user_data(r);
    struct wl_resource *res = wl_resource_create(c, &wl_callback_interface, 1,
                                                 cb);
    if (res == NULL)
        return;
    if (s->frame_cb != NULL)
        wl_resource_destroy(s->frame_cb);
    s->frame_cb = res;
}
static void s_set_region(struct wl_client *c, struct wl_resource *r, struct wl_resource *reg)
{ (void)c;(void)r;(void)reg; }
static struct wl_resource *g_focus;
static struct wl_resource *g_last_surface;

static void focus_surface(struct wl_resource *sf)
{
    if (g_keyboard == NULL || sf == NULL || g_focus == sf)
        return;
    struct wl_array keys;

    wl_array_init(&keys);
    if (g_focus != NULL)
        wl_keyboard_send_leave(g_keyboard, ++g_key_serial, g_focus);
    wl_keyboard_send_enter(g_keyboard, ++g_key_serial, sf, &keys);
    wl_keyboard_send_modifiers(g_keyboard, ++g_key_serial, 0, 0, 0, 0);
    wl_array_release(&keys);
    g_focus = sf;
}

static void s_commit(struct wl_client *c, struct wl_resource *r)
{
    (void)c;
    struct surface *s = wl_resource_get_user_data(r);

    if (s->buffer != NULL) {
        struct wl_shm_buffer *b = wl_shm_buffer_get(s->buffer);

        if (b != NULL) {
            int w = (int) wl_shm_buffer_get_width(b);
            int h = (int) wl_shm_buffer_get_height(b);
            int stride = (int) wl_shm_buffer_get_stride(b);

            wl_shm_buffer_begin_access(b);
            uint8_t *data = wl_shm_buffer_get_data(b);
            if (data != NULL) {
                if (canvas == NULL || w != canvas_w || h != canvas_h) {
                    if (canvas != NULL)
                        munmap(canvas, (size_t) canvas_w * canvas_h * 4);
                    if (canvas_fd >= 0)
                        close(canvas_fd);
                    canvas_fd = memfd_create("canvas", 0);
                    if (canvas_fd >= 0
                        && ftruncate(canvas_fd, (size_t) w * h * 4) == 0)
                        canvas = mmap(0, (size_t) w * h * 4,
                                      PROT_READ | PROT_WRITE, MAP_SHARED,
                                      canvas_fd, 0);
                    else
                        canvas = NULL;
                    canvas_w = w;
                    canvas_h = h;
                }
                if (canvas != NULL && canvas != MAP_FAILED) {
                    for (int y = 0; y < h; y++)
                        memcpy((uint8_t *) canvas + (size_t) y * w * 4,
                               data + (size_t) y * stride, (size_t) w * 4);
                    sys_display_present(canvas_fd, 50, 50, w, h);
                }
            }
            wl_shm_buffer_end_access(b);
            /* The pixels live in the compositor canvas now. */
            wl_buffer_send_release(s->buffer);
        }
    }
    g_last_surface = s->res;
    focus_surface(s->res);
    if (s->frame_cb != NULL) {
        wl_callback_send_done(s->frame_cb, now_ms());
        wl_resource_destroy(s->frame_cb);
        s->frame_cb = NULL;
    }
}
static void s_transform(struct wl_client *c, struct wl_resource *r, int32_t t) { (void)c;(void)r;(void)t; }
static void s_scale(struct wl_client *c, struct wl_resource *r, int32_t s) { (void)c;(void)r;(void)s; }
static void s_damage_buffer(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h)
{ (void)c;(void)r;(void)x;(void)y;(void)w;(void)h; }
static void s_offset(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y) { (void)c;(void)r;(void)x;(void)y; }

static const struct wl_surface_interface surface_impl = {
    s_destroy, s_attach, s_damage, s_frame, s_set_region, s_set_region,
    s_commit, s_transform, s_scale, s_damage_buffer, s_offset
};

static void surface_free(struct wl_resource *r) { free(wl_resource_get_user_data(r)); }

static void comp_create_surface(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
    (void)r;
    struct surface *s = calloc(1, sizeof(*s));
    struct wl_resource *res = wl_resource_create(c, &wl_surface_interface,
                                                 wl_resource_get_version(r), id);

    s->res = res;
    wl_resource_set_implementation(res, &surface_impl, s, surface_free);
}
static void region_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void region_add(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h)
{ (void)c;(void)r;(void)x;(void)y;(void)w;(void)h; }
static const struct wl_region_interface region_impl = { region_destroy, region_add, region_add };
static void comp_create_region(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
    (void)r;
    struct wl_resource *res = wl_resource_create(c, &wl_region_interface, 1, id);
    wl_resource_set_implementation(res, &region_impl, NULL, NULL);
}
static const struct wl_compositor_interface comp_impl = { comp_create_surface, comp_create_region };

static void bind_compositor(struct wl_client *c, void *data, uint32_t ver, uint32_t id)
{
    (void)data;
    struct wl_resource *r = wl_resource_create(c, &wl_compositor_interface, ver, id);
    wl_resource_set_implementation(r, &comp_impl, NULL, NULL);
}



static void kb_release(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static const struct wl_keyboard_interface keyboard_impl = { kb_release };

static uint32_t now_ms(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t) (ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}
static int ascii_evdev(uint8_t ch)
{
    if (ch >= 'a' && ch <= 'z') return 30 + (ch - 'a');
    if (ch >= 'A' && ch <= 'Z') return 30 + (ch - 'A');
    if (ch >= '1' && ch <= '9') return 2 + (ch - '1');
    if (ch == '0') return 11;
    switch (ch) { case ' ': return 57; case '\n': case '\r': return 28;
        case '\b': case 0x7f: return 14; case '\t': return 15; }
    return 0;
}
static void kb_send_key(uint8_t ch)
{
    int code = ascii_evdev(ch);
    if (g_keyboard == NULL || code <= 0) return;
    uint32_t t = now_ms();
    wl_keyboard_send_key(g_keyboard, ++g_key_serial, t, (uint32_t) code,
                         WL_KEYBOARD_KEY_STATE_PRESSED);
    wl_keyboard_send_key(g_keyboard, ++g_key_serial, t, (uint32_t) code,
                         WL_KEYBOARD_KEY_STATE_RELEASED);
}
static void subscribe_keys(void)
{
    g_tty_ep = sys_service_endpoint(6);
    if (g_tty_ep < 0) return;
    g_notify_ep = sys_ep_create();
    if (g_notify_ep < 0) return;
    int64_t sh = sys_handle_dup(g_notify_ep);
    if (sh < 0) return;
    sys_ipc_msg_t m; memset(&m, 0, sizeof(m));
    m.tag = TTY_KEY_SUBSCRIBE; m.xfer[0] = (uint64_t) sh; m.xfer_count = 1;
    int r = sys_ipc_send(g_tty_ep, &m);
    printf("compositor: seat tty_ep=%d notify_ep=%d sub=%d\n",
           (int) g_tty_ep, (int) g_notify_ep, r);
    fflush(stdout);
}

static void seat_get_pointer(struct wl_client *c, struct wl_resource *r, uint32_t id)
{ (void)r; struct wl_resource *res = wl_resource_create(c, &wl_pointer_interface, 1, id); wl_resource_set_implementation(res, NULL, NULL, NULL); }
static void seat_get_keyboard(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
    struct wl_resource *res = wl_resource_create(c, &wl_keyboard_interface,
                                                 wl_resource_get_version(r), id);
    if (res == NULL) return;
    wl_resource_set_implementation(res, &keyboard_impl, NULL, NULL);
    g_keyboard = res;
    int fd = memfd_create("keymap", 0);
    if (fd >= 0 && ftruncate(fd, 1) == 0) {
        /* No shared xkb data in the guest; clients fall back to raw codes. */
        wl_keyboard_send_keymap(res, WL_KEYBOARD_KEYMAP_FORMAT_NO_KEYMAP, fd, 1);
    }
    if (fd >= 0) close(fd);
    wl_keyboard_send_repeat_info(res, 25, 400);
    wl_keyboard_send_modifiers(res, ++g_key_serial, 0, 0, 0, 0);
    focus_surface(g_last_surface);
}
static void seat_get_touch(struct wl_client *c, struct wl_resource *r, uint32_t id)
{ (void)r; struct wl_resource *res = wl_resource_create(c, &wl_touch_interface, 1, id); wl_resource_set_implementation(res, NULL, NULL, NULL); }
static void seat_release(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static const struct wl_seat_interface seat_impl = { seat_get_pointer, seat_get_keyboard, seat_get_touch, seat_release };
static void bind_seat(struct wl_client *c, void *data, uint32_t ver, uint32_t id)
{
    (void)data;
    struct wl_resource *r = wl_resource_create(c, &wl_seat_interface, ver, id);
    wl_resource_set_implementation(r, &seat_impl, NULL, NULL);
    if (ver >= 2)
        wl_seat_send_name(r, "seat0");
    wl_seat_send_capabilities(r, WL_SEAT_CAPABILITY_KEYBOARD);
}

static void out_release(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static const struct wl_output_interface output_impl = { out_release };
static void bind_output(struct wl_client *c, void *data, uint32_t ver, uint32_t id)
{
    (void)data;
    struct wl_resource *r = wl_resource_create(c, &wl_output_interface, ver, id);
    wl_resource_set_implementation(r, &output_impl, NULL, NULL);
    wl_output_send_geometry(r, 0, 0, 320, 200, WL_OUTPUT_SUBPIXEL_UNKNOWN,
                            "HanOS", "fb", WL_OUTPUT_TRANSFORM_NORMAL);
    wl_output_send_mode(r, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED,
                        800, 600, 60000);
    if (ver >= 2) wl_output_send_scale(r, 1);
    if (ver >= 4) wl_output_send_name(r, "WL-1");
    if (ver >= 4) wl_output_send_description(r, "HanOS framebuffer");
    wl_output_send_done(r);
}

int main(void)
{
    setenv("XDG_RUNTIME_DIR", "/data", 1);
    struct wl_display *wd = wl_display_create();
    if (wd == NULL) { printf("compositor: create FAIL\n"); return 1; }
    wl_display_init_shm(wd);
    if (!wl_global_create(wd, &wl_compositor_interface, 4, NULL, bind_compositor)
        || !wl_global_create(wd, &wl_output_interface, 4, NULL, bind_output)
        || !wl_global_create(wd, &wl_seat_interface, 7, NULL, bind_seat)) {
        printf("compositor: global FAIL\n"); return 1;
    }
    unlink("/data/wl0");
    int sfd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strcpy(sa.sun_path, "/data/wl0");
    if (sfd < 0 || bind(sfd, (struct sockaddr *)&sa, sizeof(sa)) != 0 || listen(sfd, 128) != 0) {
        printf("compositor: bind FAIL\n"); return 1;
    }
    if (wl_display_add_socket_fd(wd, sfd) != 0) { printf("compositor: add_socket FAIL\n"); return 1; }
    printf("compositor: ready\n"); fflush(stdout);
    subscribe_keys();
    for (;;) {
        if (g_notify_ep >= 0) {
            sys_ipc_msg_t km;
            while (sys_ipc_recv_timeout(g_notify_ep, &km, 0) == 0) {
                if (km.tag == TTY_KEY)
                    kb_send_key((uint8_t) km.words[0]);
            }
        }
        wl_display_flush_clients(wd);
        wl_event_loop_dispatch(wl_display_get_event_loop(wd), 50);
    }
    return 0;
}
