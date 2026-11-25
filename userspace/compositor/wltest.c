#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <poll.h>
#include <wayland-client.h>

static struct wl_compositor *comp;
static struct wl_shm *shm;
static struct wl_seat *seat;

static void kb_keymap(void *d, struct wl_keyboard *k, uint32_t fmt, int32_t fd,
                      uint32_t size)
{
    (void) d; (void) k;
    printf("wltest: keymap format=%u size=%u\n", fmt, size);
    fflush(stdout);
    if (fd >= 0) close(fd);
}
static void kb_enter(void *d, struct wl_keyboard *k, uint32_t s,
                     struct wl_surface *sf, struct wl_array *keys)
{
    (void) d; (void) k; (void) s; (void) sf; (void) keys;
}
static void kb_leave(void *d, struct wl_keyboard *k, uint32_t s,
                     struct wl_surface *sf)
{
    (void) d; (void) k; (void) s; (void) sf;
}
static void kb_key(void *d, struct wl_keyboard *k, uint32_t s, uint32_t t,
                   uint32_t key, uint32_t st)
{
    (void) d; (void) k;
    printf("wltest: key serial=%u time=%u code=%u state=%u\n", s, t, key, st);
    fflush(stdout);
}
static void kb_mod(void *d, struct wl_keyboard *k, uint32_t s, uint32_t a,
                   uint32_t b, uint32_t c, uint32_t g)
{
    (void) d; (void) k; (void) s; (void) a; (void) b; (void) c; (void) g;
}
static void kb_rep(void *d, struct wl_keyboard *k, int32_t rate, int32_t delay)
{
    (void) d; (void) k;
    printf("wltest: repeat rate=%d delay=%d\n", rate, delay);
    fflush(stdout);
}
static const struct wl_keyboard_listener kbl = {
    kb_keymap, kb_enter, kb_leave, kb_key, kb_mod, kb_rep
};

static void seat_caps(void *d, struct wl_seat *s, uint32_t caps)
{
    (void) d;
    printf("wltest: seat caps=0x%x\n", caps);
    fflush(stdout);
    if (caps & WL_SEAT_CAPABILITY_KEYBOARD) {
        struct wl_keyboard *kb = wl_seat_get_keyboard(s);
        wl_keyboard_add_listener(kb, &kbl, NULL);
    }
}
static void seat_name(void *d, struct wl_seat *s, const char *n)
{
    (void) d; (void) s; (void) n;
}
static const struct wl_seat_listener sl = { seat_caps, seat_name };

static void g(void *d, struct wl_registry *r, uint32_t name, const char *iface,
              uint32_t v)
{
    (void) d; (void) v;
    if (!strcmp(iface, wl_compositor_interface.name))
        comp = wl_registry_bind(r, name, &wl_compositor_interface, 4);
    else if (!strcmp(iface, wl_shm_interface.name))
        shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, wl_seat_interface.name))
        seat = wl_registry_bind(r, name, &wl_seat_interface, 7);
}
static void rm(void *d, struct wl_registry *r, uint32_t n)
{
    (void) d; (void) r; (void) n;
}
static const struct wl_registry_listener rl = { g, rm };

int main(void)
{
    setenv("XDG_RUNTIME_DIR", "/data", 1);
    struct wl_display *d = NULL;
    for (int i = 0; i < 50 && d == NULL; i++) {
        d = wl_display_connect("/data/wl0");
        if (d == NULL)
            usleep(100 * 1000);
    }
    if (d == NULL) {
        printf("wltest: connect FAIL\n");
        return 1;
    }
    struct wl_registry *reg = wl_display_get_registry(d);
    wl_registry_add_listener(reg, &rl, NULL);
    for (int i = 0; i < 20; i++) {
        if (wl_display_roundtrip(d) < 0)
            break;
        if (comp != NULL && shm != NULL && seat != NULL)
            break;
        usleep(100 * 1000);
    }
    if (comp == NULL || shm == NULL || seat == NULL) {
        printf("wltest: globals FAIL c=%p s=%p seat=%p\n",
               (void *) comp, (void *) shm, (void *) seat);
        return 1;
    }
    wl_seat_add_listener(seat, &sl, NULL);

    int w = 160, h = 120, stride = w * 4, size = stride * h;
    int fd = memfd_create("surf", 0);
    ftruncate(fd, size);
    uint32_t *px = mmap(0, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int border = (x < 4 || y < 4 || x >= w - 4 || y >= h - 4);
            px[y * w + x] = border ? 0xFFE03030 : 0xFF5070C0;
        }
    }
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, w, h, stride,
                                                      WL_SHM_FORMAT_ARGB8888);
    struct wl_surface *sf = wl_compositor_create_surface(comp);
    wl_surface_attach(sf, buf, 0, 0);
    wl_surface_damage_buffer(sf, 0, 0, w, h);
    wl_surface_commit(sf);
    if (wl_display_roundtrip(d) < 0) {
        printf("wltest: roundtrip FAIL\n");
        return 1;
    }
    printf("wltest: shm PASS\n");
    fflush(stdout);

    struct pollfd pfd = { .fd = wl_display_get_fd(d), .events = POLLIN };
    for (int i = 0; i < 300; i++) {
        if (poll(&pfd, 1, 100) > 0 && (pfd.revents & POLLIN)) {
            if (wl_display_dispatch(d) < 0)
                break;
        } else {
            wl_display_flush(d);
        }
    }
    printf("wltest: done\n");
    fflush(stdout);
    return 0;
}
