/* screendraw: see screendraw.h. Landscape coordinates throughout (MPC's 1280x800 UI); a portrait buffer (the panel
 * of a Gen1 MPC) is turned a quarter, so a landscape column is one contiguous run of a buffer row.
 *
 * Two targets:
 * - SOURCE: the buffer MPC itself draws its UI into, found among this process's own DRM mappings (a plugin runs inside
 *   MPC). MPC repaints only what changes there, and copies it to the screen each frame, so pixels written into it
 *   reach the screen with MPC's own frames: no race with page flips.
 * - SCREEN: the framebuffer on screen, followed through page flips with our own DRM file (GETFB2 + MAP_DUMB). MPC
 *   copies its whole UI into the next buffer before each flip, so this has to redraw right after every flip.
 * AUTO uses SOURCE when the canvas is found there. */
#define _GNU_SOURCE
#include "screendraw.h"
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

/* ---- the few DRM/KMS ioctls used, from the Linux uapi (drm.h, drm_mode.h): stable kernel ABI ---- */
struct drm_mode_card_res {
    uint64_t fb_id_ptr, crtc_id_ptr, connector_id_ptr, encoder_id_ptr;
    uint32_t count_fbs, count_crtcs, count_connectors, count_encoders;
    uint32_t min_width, max_width, min_height, max_height;
};
struct drm_mode_modeinfo {
    uint32_t clock;
    uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
    uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan;
    uint32_t vrefresh, flags, type;
    char name[32];
};
struct drm_mode_crtc {
    uint64_t set_connectors_ptr;
    uint32_t count_connectors;
    uint32_t crtc_id, fb_id, x, y, gamma_size, mode_valid;
    struct drm_mode_modeinfo mode;
};
struct drm_mode_fb_cmd2 {
    uint32_t fb_id, width, height, pixel_format, flags;
    uint32_t handles[4], pitches[4], offsets[4];
    uint64_t modifier[4];
};
struct drm_mode_map_dumb { uint32_t handle, pad; uint64_t offset; };
struct drm_gem_close { uint32_t handle, pad; };
#define DRM_IOCTL_GEM_CLOSE         _IOW('d', 0x09, struct drm_gem_close)
#define DRM_IOCTL_DROP_MASTER       _IO('d', 0x1f)
#define DRM_IOCTL_MODE_GETRESOURCES _IOWR('d', 0xA0, struct drm_mode_card_res)
#define DRM_IOCTL_MODE_GETCRTC      _IOWR('d', 0xA1, struct drm_mode_crtc)
#define DRM_IOCTL_MODE_MAP_DUMB     _IOWR('d', 0xB3, struct drm_mode_map_dumb)
#define DRM_IOCTL_MODE_GETFB2       _IOWR('d', 0xCE, struct drm_mode_fb_cmd2)
#define DRM_MODE_FB_MODIFIERS (1 << 1)
#define FOURCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

#define TICK_US 2000                 /* how often the screen is checked for a flip */
#define NMAPS 4
#define REMAP_NS 2000000000LL        /* framebuffer ids can be reused: drop the mappings this often */
#define RESCAN_NS 500000000LL        /* while the canvas isn't found, look for it this often */
#define CHECK_NS 100000000LL         /* with no flip and no frame due, look at the buffer this often */
#define MIN_RUN 64                   /* the shortest marker run taken for a canvas edge */
#define WIT_X 8                      /* witnesses: a WIT_X x WIT_Y grid of pixels written one bit off the canvas (invisible), */
#define WIT_Y 6                      /* so any repaint by MPC, even with the canvas's own colours, shows as a change */

/* A 32-bit XRGB buffer seen in landscape. turn 90: landscape x runs up the buffer's rows, y along a row. */
typedef struct { uint8_t *base; size_t len; uint32_t pitch; int turn, lw, lh; } surf_t;

/* A buffer drawn into, and what was last written there: shadow holds the canvas as copied (valid while it still
 * matches the buffer), svis the rows visible then. Only the difference from it is copied. */
struct fbmap {
    uint32_t fb_id;
    surf_t s;
    uint64_t serial;
    uint32_t *shadow;
    uint8_t *svis;
    int valid;
    uint32_t wit[WIT_X * WIT_Y];
};

enum { T_AUTO, T_SCREEN, T_SOURCE };

static struct {
    pthread_mutex_t mtx;             /* everything below */
    pthread_t thread;
    int running;
    void *owner[8];
    scr_draw_fn fn[8];
    int nowners;
    int fps, want;                   /* want: T_AUTO / T_SCREEN / T_SOURCE */

    int fd;                          /* our own file on the display card */
    uint32_t crtc;
    struct fbmap maps[NMAPS];        /* SCREEN: the framebuffers seen on screen */
    int nmaps;
    long long mapped_at;
    struct fbmap src;                /* SOURCE: MPC's own mapping (never unmapped by us) */
    int use_src;
    int lw, lh;                      /* the screen, landscape */

    int found;                       /* the canvas: drawable area at (cx, cy), cw x ch, markers at cx-1 and cx+cw */
    int cx, cy, cw, ch;
    long long scanned_at;
    uint8_t *vis;                    /* per canvas row: both markers there */
    int nvis;
    uint32_t *col;
    scr_canvas_t c;
    uint64_t serial;                 /* frames drawn */

    char err[64];
    long long stat_at;
    int frames, blits, repaints, flips, flipped, aborts;
    double blit_ns, draw_ns;
    uint32_t last_fb;
} S = { .mtx = PTHREAD_MUTEX_INITIALIZER, .fd = -1, .fps = 30 };

static long long now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000000000LL + t.tv_nsec;
}

static long long cpu_ns(void)          /* this thread's CPU time: what drawing and copying cost, not time preempted */
{
    struct timespec t;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return (long long)t.tv_sec * 1000000000LL + t.tv_nsec;
}

/* ---- buffers ---- */

static void set_geometry(surf_t *s, uint32_t w, uint32_t h, uint32_t pitch)
{
    s->pitch = pitch;
    s->turn = h > w ? 90 : 0;
    s->lw = (int)(h > w ? h : w);
    s->lh = (int)(h > w ? w : h);
}

static inline uint32_t *at(const surf_t *s, int x, int y)
{
    if (s->turn) return (uint32_t *)(s->base + (size_t)(s->lw - 1 - x) * s->pitch) + y;
    return (uint32_t *)(s->base + (size_t)y * s->pitch) + x;
}

/* Rows y0..y0+n-1 of landscape column x, into out (one burst read where it's contiguous: buffers are uncached). */
static void read_col(const surf_t *s, int x, int y0, int n, uint32_t *out)
{
    if (s->turn) { memcpy(out, at(s, x, y0), (size_t)n * 4); return; }
    for (int i = 0; i < n; i++) out[i] = *at(s, x, y0 + i);
}

static void write_col(const surf_t *s, int x, int y0, int n, const uint32_t *in)
{
    if (s->turn) { memcpy(at(s, x, y0), in, (size_t)n * 4); return; }
    for (int i = 0; i < n; i++) *at(s, x, y0 + i) = in[i];
}

static void forget(struct fbmap *m)
{
    free(m->shadow);
    free(m->svis);
    m->shadow = NULL;
    m->svis = NULL;
    m->valid = 0;
}

static void unmap(struct fbmap *m)
{
    forget(m);
    munmap(m->s.base, m->s.len);
}

static void unmap_all(void)
{
    for (int i = 0; i < S.nmaps; i++) unmap(&S.maps[i]);
    S.nmaps = 0;
}

static void close_display(const char *why)
{
    unmap_all();
    if (S.fd >= 0) close(S.fd);
    S.fd = -1;
    S.found = 0;
    snprintf(S.err, sizeof S.err, "%s", why);
}

#ifdef SCREENDRAW_TEST
/* Tests only: SCREENDRAW_FAKE_FB="file,width,height" is the display (XRGB8888, pitch = width * 4). */
static int fake_fb(struct fbmap *m)
{
    const char *spec = getenv("SCREENDRAW_FAKE_FB");
    char path[256];
    unsigned w, h;
    if (!spec || sscanf(spec, "%255[^,],%u,%u", path, &w, &h) != 3) return -1;
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) return -1;
    size_t len = (size_t)w * h * 4;
    void *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return -1;
    *m = (struct fbmap){ .fb_id = 1, .s = { .base = p, .len = len } };
    set_geometry(&m->s, w, h, w * 4);
    return 0;
}
#endif

static int open_display(void)
{
    for (int i = 0; i < 8; i++) {
        char p[32];
        snprintf(p, sizeof p, "/dev/dri/card%d", i);
        int fd = open(p, O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        /* A card with no master makes its first opener master, which would keep MPC from taking the display. MPC is
         * master by the time a plugin loads, so this fails harmlessly; it's here in case it ever isn't. */
        ioctl(fd, DRM_IOCTL_DROP_MASTER, 0);
        uint32_t ids[16];
        struct drm_mode_card_res r = { .crtc_id_ptr = (uint64_t)(uintptr_t)ids, .count_crtcs = 16 };
        if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &r) == 0) {
            uint32_t n = r.count_crtcs < 16 ? r.count_crtcs : 16;
            for (uint32_t k = 0; k < n; k++) {
                struct drm_mode_crtc c = { .crtc_id = ids[k] };
                if (ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &c) == 0 && c.fb_id && c.mode_valid) {
                    S.fd = fd;
                    S.crtc = ids[k];
                    return 0;
                }
            }
        }
        close(fd);
    }
    snprintf(S.err, sizeof S.err, "no lit display");
    return -1;
}

static int map_fb(uint32_t fb_id, struct fbmap *m)
{
    struct drm_mode_fb_cmd2 f = { .fb_id = fb_id };
    if (ioctl(S.fd, DRM_IOCTL_MODE_GETFB2, &f)) { snprintf(S.err, sizeof S.err, "GETFB2 failed"); return -1; }
    for (int i = 1; i < 4; i++)
        if (f.handles[i] && f.handles[i] != f.handles[0]) {
            struct drm_gem_close gc = { .handle = f.handles[i] };
            ioctl(S.fd, DRM_IOCTL_GEM_CLOSE, &gc);
        }
    int rc = 0;
    if (!f.handles[0]) { snprintf(S.err, sizeof S.err, "no buffer handle (needs root)"); return -1; }
    if (f.pixel_format != FOURCC('X', 'R', '2', '4') && f.pixel_format != FOURCC('A', 'R', '2', '4')) rc = -1;
    if ((f.flags & DRM_MODE_FB_MODIFIERS) && f.modifier[0]) rc = -1;
    if (f.offsets[0] || f.pitches[0] < f.width * 4) rc = -1;
    if (rc) snprintf(S.err, sizeof S.err, "unsupported framebuffer format");
    struct drm_mode_map_dumb md = { .handle = f.handles[0] };
    void *p = MAP_FAILED;
    size_t len = (size_t)f.pitches[0] * f.height;
    if (!rc && ioctl(S.fd, DRM_IOCTL_MODE_MAP_DUMB, &md) == 0)
        p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, S.fd, (off_t)md.offset);
    struct drm_gem_close gc = { .handle = f.handles[0] };   /* the mapping keeps the buffer alive */
    ioctl(S.fd, DRM_IOCTL_GEM_CLOSE, &gc);
    if (rc) return -1;
    if (p == MAP_FAILED) { snprintf(S.err, sizeof S.err, "can't map the framebuffer"); return -1; }
    *m = (struct fbmap){ .fb_id = fb_id, .s = { .base = p, .len = len } };
    set_geometry(&m->s, f.width, f.height, f.pitches[0]);
    return 0;
}

/* SCREEN: the framebuffer on screen now, mapped. */
static struct fbmap *front(void)
{
#ifdef SCREENDRAW_TEST
    if (getenv("SCREENDRAW_FAKE_FB")) {
        if (!S.nmaps && fake_fb(&S.maps[0]) == 0) S.nmaps = 1;
        return S.nmaps ? &S.maps[0] : NULL;
    }
#endif
    if (S.fd < 0 && open_display()) return NULL;
    struct drm_mode_crtc c = { .crtc_id = S.crtc };
    if (ioctl(S.fd, DRM_IOCTL_MODE_GETCRTC, &c) || !c.fb_id || !c.mode_valid) {
        close_display("display went away");
        return NULL;
    }
    if (c.fb_id != S.last_fb) { S.flips++; S.flipped = 1; S.last_fb = c.fb_id; }
    long long t = now_ns();
    if (t - S.mapped_at > REMAP_NS) { unmap_all(); S.mapped_at = t; }
    for (int i = 0; i < S.nmaps; i++)
        if (S.maps[i].fb_id == c.fb_id) return &S.maps[i];
    if (S.nmaps == NMAPS) {
        unmap(&S.maps[0]);
        memmove(&S.maps[0], &S.maps[1], sizeof S.maps[0] * (NMAPS - 1));
        S.nmaps--;
    }
    if (map_fb(c.fb_id, &S.maps[S.nmaps])) return NULL;
    return &S.maps[S.nmaps++];
}

/* ---- finding the canvas ---- */

static inline int near(uint32_t px, uint32_t want)
{
    for (int s = 0; s < 24; s += 8) {
        int d = (int)(px >> s & 255) - (int)(want >> s & 255);
        if (d < -6 || d > 6) return 0;
    }
    return 1;
}

/* The longest run in col[0..n) alternating a, b, a, ... (starting on a): its start and length. */
static int marker_run(const uint32_t *col, int n, uint32_t a, uint32_t b, int *start)
{
    int best = 0, len = 0;
    for (int y = 0; y < n; y++) {
        if (len && near(col[y], len & 1 ? b : a)) len++;
        else len = near(col[y], a) ? 1 : 0;
        if (len > best) { best = len; *start = y - len + 1; }
    }
    return best;
}

/* Look for the canvas's marker edges in a buffer. Sets cx, cy, cw, ch and returns 1 if found. */
static int find_canvas(const surf_t *s)
{
    uint32_t *col = malloc((size_t)s->lh * 4);
    if (!col) return 0;
    int lx = -1, ly = 0, ln = 0, ok = 0;
    for (int x = 0; x < s->lw && !ok; x++) {
        read_col(s, x, 0, s->lh, col);
        int st = 0, n;
        if (lx < 0) {
            if ((n = marker_run(col, s->lh, SCR_MARK_L_A, SCR_MARK_L_B, &st)) >= MIN_RUN) { lx = x; ly = st; ln = n; }
        } else if ((n = marker_run(col, s->lh, SCR_MARK_R_A, SCR_MARK_R_B, &st)) >= MIN_RUN && abs(st - ly) <= 1) {
            /* the rows both edges share; the first and last of them are the canvas's top and bottom lines */
            int y0 = st > ly ? st : ly, y1 = (st + n < ly + ln ? st + n : ly + ln);
            S.cx = lx + 1; S.cw = x - lx - 1;
            S.cy = y0 + 1; S.ch = y1 - y0 - 2;
            ok = S.cw > 8 && S.ch > 8;
        }
    }
    free(col);
    return ok;
}

/* SOURCE: MPC's own CPU mappings of DRM buffers (rw, shared, at least 1 MB, not ours), each tried in both
 * orientations of the screen's size; the first holding the canvas becomes the target. Returns 1 if found. */
static int find_source(void)
{
    if (!S.lw) return 0;             /* the screen's size, from the display, comes first */
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    char line[512];
    int ok = 0;
    while (!ok && fgets(line, sizeof line, f)) {
        unsigned long a, e;
        char perms[8];
        const char *pat = "/dev/dri/";
#ifdef SCREENDRAW_TEST
        if (getenv("SCREENDRAW_FAKE_SRC")) pat = getenv("SCREENDRAW_FAKE_SRC");   /* a file the test maps as MPC would */
#endif
        if (!strstr(line, pat) || sscanf(line, "%lx-%lx %7s", &a, &e, perms) != 3) continue;
        if (perms[0] != 'r' || perms[1] != 'w' || perms[3] != 's' || e - a < (size_t)S.lw * S.lh * 4) continue;
        int ours = 0;
        for (int i = 0; i < S.nmaps; i++) ours |= (unsigned long)(uintptr_t)S.maps[i].s.base == a;
        if (ours) continue;
        for (int g = 0; g < 2 && !ok; g++) {
            surf_t s = { .base = (uint8_t *)(uintptr_t)a, .len = e - a };
            if (g == 0) set_geometry(&s, (uint32_t)S.lw, (uint32_t)S.lh, (uint32_t)S.lw * 4);   /* landscape */
            else set_geometry(&s, (uint32_t)S.lh, (uint32_t)S.lw, (uint32_t)S.lh * 4);          /* portrait */
            if (find_canvas(&s)) {
                forget(&S.src);
                S.src = (struct fbmap){ .s = s };
                ok = 1;
            }
        }
    }
    fclose(f);
    return ok;
}

/* MPC's mapping is MPC's: before using it, make sure it is still mapped (mincore fails with ENOMEM if not). */
static int src_alive(void)
{
    unsigned char v[1];
    long pg = sysconf(_SC_PAGESIZE);
    uintptr_t a = (uintptr_t)S.src.s.base & ~(uintptr_t)(pg - 1);
    return mincore((void *)a, (size_t)pg, v) == 0 && mincore((void *)(a + ((S.src.s.len - 1) & ~(size_t)(pg - 1))), (size_t)pg, v) == 0;
}

static void adopt_canvas(void)
{
    free(S.c.px); free(S.vis); free(S.col);
    S.c.w = S.cw; S.c.h = S.ch;
    S.c.px = calloc((size_t)S.cw * S.ch, 4);
    S.vis = calloc((size_t)S.ch, 1);
    S.col = malloc((size_t)(S.ch + 2) * 4);
    if (!S.c.px || !S.vis || !S.col) { S.found = 0; snprintf(S.err, sizeof S.err, "out of memory"); return; }
    for (size_t i = 0; i < (size_t)S.cw * S.ch; i++) S.c.px[i] = SCR_CANVAS_BG;
    for (int i = 0; i < S.nmaps; i++) forget(&S.maps[i]);
    S.found = 1;
    S.serial++;                      /* the blank canvas counts as a frame: drawn wherever it's missing */
}

/* Look for the canvas: in MPC's own buffer (unless SCREEN is asked for), else on screen. */
static void scan(struct fbmap *m)
{
    S.found = 0;
    S.use_src = 0;
    S.scanned_at = now_ns();
    if (m) { S.lw = m->s.lw; S.lh = m->s.lh; }
    if (S.want != T_SCREEN && find_source()) S.use_src = 1;
    else if (S.want != T_SOURCE && m && find_canvas(&m->s)) S.use_src = 0;
    else { snprintf(S.err, sizeof S.err, "canvas not on screen"); return; }
    adopt_canvas();
}

/* Which canvas rows have both markers in this buffer. Returns how many do. */
static int visible(const surf_t *s)
{
    uint32_t *l = S.col;
    int n = S.ch + 2, y0 = S.cy - 1, cnt = 0;      /* the marker columns include the top and bottom lines */
    read_col(s, S.cx - 1, y0, n, l);
    for (int i = 1; i <= S.ch; i++) S.vis[i - 1] = near(l[i], i & 1 ? SCR_MARK_L_B : SCR_MARK_L_A);   /* row 0 is a */
    read_col(s, S.cx + S.cw, y0, n, l);
    for (int i = 1; i <= S.ch; i++)
        if (S.vis[i - 1] && !near(l[i], i & 1 ? SCR_MARK_R_B : SCR_MARK_R_A)) S.vis[i - 1] = 0;
    for (int i = 0; i < S.ch; i++) cnt += S.vis[i];
    return cnt;
}

static inline int wit_x(int i) { return (S.cw - 1) * i / (WIT_X - 1); }
static inline int wit_y(int j) { return (S.ch - 1) * j / (WIT_Y - 1); }

/* Has MPC painted over what was last copied into this buffer? Witnesses on rows visible then and now. */
static int intact(const struct fbmap *m)
{
    if (!m->valid) return 0;
    for (int i = 0; i < WIT_X; i++)
        for (int j = 0; j < WIT_Y; j++) {
            int x = wit_x(i), y = wit_y(j);
            if (S.vis[y] && m->svis[y] && (*at(&m->s, S.cx + x, S.cy + y) & 0xFFFFFF) != m->wit[i * WIT_Y + j]) return 0;
        }
    return 1;
}

/* Are the canvas's markers still in this buffer? Two pixels: the left marker at the first visible row and the
 * right one at the last. MPC opening another page paints over them, so this is checked before every column. */
static int still_ours(const surf_t *s, int vy0, int vy1)
{
    return near(*at(s, S.cx - 1, S.cy + vy0), (vy0 + 1) & 1 ? SCR_MARK_L_B : SCR_MARK_L_A) &&
           near(*at(s, S.cx + S.cw, S.cy + vy1), (vy1 + 1) & 1 ? SCR_MARK_R_B : SCR_MARK_R_A);
}

/* Copy the canvas into a buffer: in full, or only each column's changed span since the last copy into it.
 * In MPC's own buffer (SOURCE) MPC may paint another page while this runs: the copy stops at the first column whose
 * check finds the markers gone, and the column written just before is covered with its neighbour from MPC's new
 * page, so at most one column of ours could linger, hidden in MPC's own pixels. */
static void blit(struct fbmap *m, int full)
{
    long long t0 = cpu_ns();
    if (!m->shadow) {
        m->shadow = malloc((size_t)S.cw * S.ch * 4);
        m->svis = malloc((size_t)S.ch);
        if (!m->shadow || !m->svis) { forget(m); return; }
        full = 1;
    }
    if (!m->valid || memcmp(m->svis, S.vis, (size_t)S.ch)) full = 1;   /* rows came into view: they need everything */
    uint32_t *row = S.col;
    int vy0 = 0, vy1 = S.ch - 1, wrote = -1;
    while (vy0 < S.ch && !S.vis[vy0]) vy0++;
    while (vy1 >= 0 && !S.vis[vy1]) vy1--;
    if (vy0 > vy1) return;
    for (int x = 0; x < S.cw; x++) {
        if (!still_ours(&m->s, vy0, vy1)) {
            if (wrote >= 0 && wrote + 1 < S.cw) {
                read_col(&m->s, S.cx + wrote + 1, S.cy, S.ch, row);
                write_col(&m->s, S.cx + wrote, S.cy, S.ch, row);
            }
            m->valid = 0;
            S.aborts++;
            return;
        }
        const uint32_t *src = S.c.px + (size_t)x * S.ch;
        uint32_t *sh = m->shadow + (size_t)x * S.ch;
        int lo = 0, hi = S.ch - 1;
        if (!full) {
            while (lo <= hi && src[lo] == sh[lo]) lo++;
            while (hi >= lo && src[hi] == sh[hi]) hi--;
            if (lo > hi) continue;
        }
        for (int y = lo; y <= hi;) {
            if (!S.vis[y]) { y++; continue; }
            int e = y;
            while (e <= hi && S.vis[e]) e++;
            for (int i = y; i < e; i++) row[i - y] = src[i] | 0xFF000000u;
            write_col(&m->s, S.cx + x, S.cy + y, e - y, row);
            wrote = x;
            y = e;
        }
        memcpy(sh + lo, src + lo, (size_t)(hi - lo + 1) * 4);
    }
    if (!still_ours(&m->s, vy0, vy1)) { m->valid = 0; S.aborts++; return; }   /* gone at the last moment */
    for (int i = 0; i < WIT_X; i++)
        for (int j = 0; j < WIT_Y; j++) {
            int x = wit_x(i), y = wit_y(j);
            uint32_t v = (S.c.px[(size_t)x * S.ch + y] ^ 1) & 0xFFFFFF;
            m->wit[i * WIT_Y + j] = v;
            if (S.vis[y]) *at(&m->s, S.cx + x, S.cy + y) = v | 0xFF000000u;
        }
    memcpy(m->svis, S.vis, (size_t)S.ch);
    m->valid = 1;
    m->serial = S.serial;
    S.blits++;
    S.blit_ns += (double)(cpu_ns() - t0);
}

/* ---- the thread ---- */

static void *run(void *arg)
{
    (void)arg;
    pthread_setname_np(pthread_self(), "screendraw");
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);   /* below MPC's own threads */
    long long next_frame = 0, checked_at = 0;
    S.stat_at = now_ns();
    pthread_mutex_lock(&S.mtx);
    while (S.running) {
        /* Every tick asks only which framebuffer is on screen (one ioctl). The buffer itself (uncached memory, slow
         * to read) is looked at when MPC flips (SCREEN), when a frame is due, and every CHECK_NS otherwise. */
        long long t = now_ns();
        S.flipped = 0;
        struct fbmap *m = front();
        if (S.found && S.use_src && !src_alive()) S.found = 0;   /* MPC let its buffer go: look again */
        struct fbmap *dst = S.found ? (S.use_src ? &S.src : m) : NULL;
        if (!S.found) {
            if (t - S.scanned_at > RESCAN_NS) scan(m);
        } else if (dst) {
            int due = S.nowners && t >= next_frame;
            if ((S.flipped && !S.use_src) || due || t - checked_at >= CHECK_NS) {
                checked_at = t;
                S.nvis = visible(&dst->s);
                if (!S.nvis) {
                    if (t - S.scanned_at > RESCAN_NS) scan(m);   /* not there: maybe it moved */
                } else {
                    if (due) {
                        next_frame = t + 1000000000LL / S.fps;
                        S.c.w = S.cw; S.c.h = S.ch;
                        long long d0 = cpu_ns();
                        if (S.fn[S.nowners - 1](S.owner[S.nowners - 1], &S.c)) { S.serial++; S.frames++; }
                        S.draw_ns += (double)(cpu_ns() - d0);
                    }
                    if (!intact(dst)) {
                        if (dst->valid) S.repaints++;
                        dst->valid = 0;
                        blit(dst, 1);
                    } else if (dst->serial != S.serial) blit(dst, 0);
                }
            }
        }
        if (t - S.stat_at >= 1000000000LL) {
            double s = (double)(t - S.stat_at) / 1e9;
            S.c.fps = (float)(S.frames / s);
            S.c.blit_ms = S.blits ? (float)(S.blit_ns / S.blits / 1e6) : 0;
            S.c.draw_ms = S.frames ? (float)(S.draw_ns / S.frames / 1e6) : 0;
            S.c.repaints_s = (int)(S.repaints / s + 0.5);
            S.c.flips_s = (int)(S.flips / s + 0.5);
            S.c.source = S.found && S.use_src;
            S.frames = S.blits = S.repaints = S.flips = 0;
            S.blit_ns = S.draw_ns = 0;
            S.stat_at = t;
        }
        pthread_mutex_unlock(&S.mtx);
        usleep(TICK_US);
        pthread_mutex_lock(&S.mtx);
    }
    close_display("stopped");
    forget(&S.src);
    S.src = (struct fbmap){ 0 };
    free(S.c.px); free(S.vis); free(S.col);
    S.c.px = NULL; S.vis = NULL; S.col = NULL;
    pthread_mutex_unlock(&S.mtx);
    return NULL;
}

void scr_attach(void *owner, scr_draw_fn fn)
{
    pthread_mutex_lock(&S.mtx);
    if (S.nowners < 8) {
        S.owner[S.nowners] = owner;
        S.fn[S.nowners++] = fn;
    }
    if (!S.running) {
        S.running = 1;
        S.err[0] = 0;
        S.scanned_at = 0;
        if (pthread_create(&S.thread, NULL, run, NULL)) S.running = 0;
    }
    pthread_mutex_unlock(&S.mtx);
}

void scr_detach(void *owner)
{
    pthread_mutex_lock(&S.mtx);
    for (int i = 0; i < S.nowners; i++)
        if (S.owner[i] == owner) {
            memmove(&S.owner[i], &S.owner[i + 1], sizeof S.owner[0] * (size_t)(S.nowners - i - 1));
            memmove(&S.fn[i], &S.fn[i + 1], sizeof S.fn[0] * (size_t)(S.nowners - i - 1));
            S.nowners--;
            break;
        }
    int stop = S.running && !S.nowners;
    if (stop) S.running = 0;
    pthread_mutex_unlock(&S.mtx);
    if (stop) pthread_join(S.thread, NULL);
}

void scr_set_fps(int fps)
{
    pthread_mutex_lock(&S.mtx);
    S.fps = fps < 1 ? 1 : fps > 60 ? 60 : fps;
    pthread_mutex_unlock(&S.mtx);
}

void scr_set_target(int target)
{
    pthread_mutex_lock(&S.mtx);
    int w = target == 1 ? T_SCREEN : target == 2 ? T_SOURCE : T_AUTO;
    if (w != S.want) { S.want = w; S.found = 0; S.scanned_at = 0; }   /* look again, for the new target */
    pthread_mutex_unlock(&S.mtx);
}

int scr_status(char *buf, int n)
{
    pthread_mutex_lock(&S.mtx);
    int r;
    if (!S.found) r = snprintf(buf, n, "%s", S.err[0] ? S.err : "starting");
    else if (!S.nvis) r = snprintf(buf, n, "%dx%d, not on screen", S.cw, S.ch);
    else r = snprintf(buf, n, "%s %dx%d  %.0f fps  draw %.2f ms  copy %.2f ms  %d flips/s",
                      S.use_src ? "SOURCE" : "SCREEN", S.cw, S.ch, S.c.fps, S.c.draw_ms, S.c.blit_ms, S.c.flips_s);
    pthread_mutex_unlock(&S.mtx);
    return r;
}
