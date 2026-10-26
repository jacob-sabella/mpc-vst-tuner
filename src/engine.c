/* Tuner: chromatic tuner insert drawn onto MPC's screen via screendraw (src/screendraw.c). The audio thread copies
 * its input into a mono ring (and mutes output if asked); pitch detection runs on the drawing thread, once a
 * frame, only while the canvas is visible.
 *
 * Pitch: YIN (de Cheveigne & Kawahara 2002) on the input decimated 4x to 11025 Hz (covers 27-1378 Hz), then the
 * period refined at 44.1 kHz around what it found, with parabolic interpolation at both stages. A 5-estimate
 * median steadies the note; the needle eases toward the cents.
 * Views: a needle gauge, or a strobe whose bands drift at a speed set by the cents offset. Synthwave palette:
 * a purple sky over a grid floor that scrolls toward the viewer. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "engine.h"
#include "screendraw.h"
#include "sdfont.h"

#define RING     8192
#define NFULL    4096
#define DEC      4
#define NDEC     (NFULL / DEC)
#define YW       512
#define YTMIN    8
#define YTMAX    400
#define GATE     0.003f
#define HOLD_S   1.5f
#define NMED     5
#define YIN_THR  0.15f

#define C_SKY_TOP 0x0B0420
#define C_SKY_HZN 0x3A0C5C
#define C_FLOOR   0x0A0218
#define C_GRID    0xB026FF
#define C_PINK    0xFF2A6D
#define C_CYAN    0x05D9E8
#define C_YELLOW  0xFFD319
#define C_ORANGE  0xFF6C11
#define C_MAGENTA 0xD62AD0
#define C_PURPLE  0x9D4EDD
#define C_TEXT    0xF5E6FF
#define C_TICK    0x9D7BD8
#define C_DIM     0x4A3366

enum { VIEW_NEEDLE, VIEW_STROBE };
static const char *NOTES[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

typedef struct {
    int16_t ring[RING];
    unsigned wpos;
    int ref, mute, view, target;
    float x[NFULL], xd[NDEC], d[YTMAX + 2];
    float med[NMED];
    int nmed;
    int has, midi;
    float hz, cents, smooth_cents;
    int hold_frames;
    float strobe_off, grid_phase;
    uint32_t *bg;                    /* the sky and the floor's fixed lines, column-major like the canvas */
    int bgw, bgh;
} tuner_t;

/* ---- YIN pitch detection ---- */

/* The period in decimated samples (interpolated), or 0 without a clear one. */
static float yin_lag(tuner_t *t)
{
    for (int tau = 1; tau <= YTMAX; tau++) {
        float sum = 0;
        for (int j = 0; j < YW; j++) {
            float v = t->xd[j] - t->xd[j + tau];
            sum += v * v;
        }
        t->d[tau] = sum;
    }
    t->d[0] = 1.0f;
    float running = 0;
    for (int tau = 1; tau <= YTMAX; tau++) {
        running += t->d[tau];
        t->d[tau] = running > 0 ? t->d[tau] * (float)tau / running : 1.0f;
    }
    for (int tau = YTMIN; tau < YTMAX; tau++) {
        if (t->d[tau] < YIN_THR) {
            while (tau + 1 <= YTMAX && t->d[tau + 1] < t->d[tau]) tau++;
            if (tau > YTMIN && tau < YTMAX) {
                float a = t->d[tau - 1], b = t->d[tau], c = t->d[tau + 1];
                float dn = a - 2 * b + c;
                float delta = dn > 1e-12f ? 0.5f * (a - c) / dn : 0;
                return (float)tau + delta;
            }
            return (float)tau;
        }
    }
    return 0;
}

/* The coarse lag times DEC, searched again at 44.1 kHz: the difference function's minimum within a few samples of it,
 * interpolated. At 11025 Hz a high note's period is some 25 samples and its interpolation is off by up to 2 cents. */
static float refine(const tuner_t *t, float coarse)
{
    enum { R = DEC + 2 };
    int c = (int)lroundf(coarse * DEC);
    if (c - R < 2 || c + R + 1 >= NFULL / 2) return coarse * DEC;
    int win = NFULL - (c + R + 1);
    if (win > 2048) win = 2048;
    float dd[2 * R + 1];
    int best = 0;
    for (int k = 0; k <= 2 * R; k++) {
        int tau = c - R + k;
        float s = 0;
        for (int j = 0; j < win; j++) {
            float v = t->x[j] - t->x[j + tau];
            s += v * v;
        }
        dd[k] = s;
        if (s < dd[best]) best = k;
    }
    if (best == 0 || best == 2 * R) return coarse * DEC;
    float a = dd[best - 1], b = dd[best], cc = dd[best + 1], dn = a - 2 * b + cc;
    return (float)(c - R + best) + (dn > 0 ? 0.5f * (a - cc) / dn : 0);
}

/* ---- drawing helpers ---- */

static uint32_t mix(uint32_t a, uint32_t b, float t)
{
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    uint32_t o = 0;
    for (int s = 0; s < 24; s += 8)
        o |= (uint32_t)((float)(a >> s & 255) * (1 - t) + (float)(b >> s & 255) * t) << s;
    return o;
}

static void vspan(scr_canvas_t *c, int x, int y0, int y1, uint32_t col)
{
    if ((unsigned)x >= (unsigned)c->w) return;
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    if (y0 < 0) y0 = 0;
    if (y1 >= c->h) y1 = c->h - 1;
    uint32_t *p = c->px + (size_t)x * c->h;
    for (int y = y0; y <= y1; y++) p[y] = col;
}

static void hspan(scr_canvas_t *c, int y, int x0, int x1, uint32_t col)
{
    if ((unsigned)y >= (unsigned)c->h) return;
    if (x0 < 0) x0 = 0;
    if (x1 >= c->w) x1 = c->w - 1;
    for (int x = x0; x <= x1; x++) c->px[(size_t)x * c->h + y] = col;
}

static int horizon(const scr_canvas_t *c) { return c->h * 68 / 100; }

/* Sky gradient, horizon glow and the floor's lines toward the vanishing point: fixed, so drawn once per canvas size. */
static int make_bg(tuner_t *d, const scr_canvas_t *c)
{
    if (d->bg && d->bgw == c->w && d->bgh == c->h) return 1;
    free(d->bg);
    d->bg = malloc((size_t)c->w * c->h * sizeof *d->bg);
    if (!d->bg) { d->bgw = d->bgh = 0; return 0; }
    d->bgw = c->w;
    d->bgh = c->h;
    int hz = horizon(c), cx = c->w / 2;
    for (int x = 0; x < c->w; x++)
        for (int y = 0; y < c->h; y++)
            d->bg[(size_t)x * c->h + y] = y < hz ? mix(C_SKY_TOP, C_SKY_HZN, (float)y / (float)hz) : C_FLOOR;
    for (int y = hz; y < c->h; y++) {
        float t = (float)(y - hz) / (float)(c->h - hz);
        uint32_t col = mix(C_FLOOR, C_GRID, 0.25f + 0.6f * t);
        for (int k = -14; k <= 14; k++) {
            int x = cx + (int)((float)k * (14.0f + 110.0f * t));
            if ((unsigned)x < (unsigned)c->w) d->bg[(size_t)x * c->h + y] = col;
        }
    }
    for (int x = 0; x < c->w; x++) {
        d->bg[(size_t)x * c->h + hz] = C_PINK;
        if (hz > 0) d->bg[(size_t)x * c->h + hz - 1] = mix(C_SKY_HZN, C_PINK, 0.5f);
    }
    return 1;
}

/* The background, plus the floor's cross lines, which move toward the viewer while a note sounds. */
static void clear(tuner_t *d, scr_canvas_t *c, int moving)
{
    if (!make_bg(d, c)) {
        for (size_t i = 0; i < (size_t)c->w * c->h; i++) c->px[i] = C_FLOOR;
        return;
    }
    memcpy(c->px, d->bg, (size_t)c->w * c->h * sizeof *c->px);
    if (moving) {
        d->grid_phase += 0.04f;
        if (d->grid_phase >= 1) d->grid_phase -= 1;
    }
    int hz = horizon(c);
    for (int i = 0; i < 12; i++) {
        float z = (float)i + 1.0f - d->grid_phase;
        if (z < 1) continue;
        int y = hz + (int)((float)(c->h - 1 - hz) / z);
        hspan(c, y, 0, c->w - 1, mix(C_FLOOR, C_GRID, 0.9f / z + 0.15f));
    }
}

/* Text with a cyan copy offset behind it, the scan-line look. */
static void glow_text(scr_canvas_t *c, int x, int y, int scale, uint32_t col, uint32_t shadow, const char *s)
{
    int o = scale / 2 > 1 ? scale / 2 : 1;
    sd_text(c, x + o, y + o, scale, shadow, s);
    sd_text(c, x, y, scale, col, s);
}

static uint32_t cent_color(float cents)
{
    float a = fabsf(cents);
    if (a < 3) return C_CYAN;
    if (a < 10) return C_YELLOW;
    return C_PINK;
}

static void note_name(const tuner_t *d, char *buf, int n)
{
    snprintf(buf, (size_t)n, "%s%d", NOTES[((d->midi % 12) + 12) % 12], d->midi / 12 - 1);
}

/* ---- needle view ---- */

static void draw_needle(tuner_t *d, scr_canvas_t *c, int dim)
{
    uint32_t cc = dim ? C_DIM : cent_color(d->smooth_cents);
    char buf[24];

    note_name(d, buf, sizeof buf);
    glow_text(c, c->w / 2 - sd_text_width(buf, 8) / 2, 16, 8, dim ? C_DIM : C_PINK, dim ? C_SKY_HZN : C_CYAN, buf);

    snprintf(buf, sizeof buf, "%+d", (int)roundf(d->smooth_cents));
    glow_text(c, c->w / 2 - sd_text_width(buf, 4) / 2, 92, 4, cc, dim ? C_SKY_HZN : C_PURPLE, buf);

    snprintf(buf, sizeof buf, "%.1f HZ", (double)d->hz);
    sd_text(c, c->w / 2 - sd_text_width(buf, 2) / 2, 132, 2, dim ? C_DIM : C_TICK, buf);

    int gy = 170, gh = 80, mid = gy + gh / 2;
    int gx0 = 40, gx1 = c->w - 40, gcx = (gx0 + gx1) / 2, gw = gx1 - gx0;

    for (int x = gx0; x <= gx1; x++) {          /* the scale: orange at the ends, through pink, to cyan in the middle */
        float t = fabsf((float)(x - gcx)) / (float)(gw / 2);
        uint32_t col = dim ? C_DIM : t < 0.1f ? C_CYAN : mix(C_MAGENTA, C_ORANGE, (t - 0.1f) / 0.9f);
        vspan(c, x, mid - 1, mid + 1, col);
    }
    for (int ct = -50; ct <= 50; ct += 10) {
        int tx = gcx + ct * gw / 100;
        int th = ct == 0 ? gh / 2 : (ct % 20 == 0 ? gh / 3 : gh / 5);
        vspan(c, tx, mid - th, mid + th, dim ? C_DIM : ct == 0 ? C_CYAN : C_TICK);
        if (ct == 0) vspan(c, tx + 1, mid - th, mid + th, dim ? C_DIM : C_CYAN);
    }

    float cl = d->smooth_cents < -50 ? -50 : d->smooth_cents > 50 ? 50 : d->smooth_cents;
    int nx = gcx + (int)(cl * (float)gw / 100.0f);
    for (int dx = -8; dx <= 8; dx++) {           /* a glow that fades out to each side */
        int a = dx < 0 ? -dx : dx;
        if (a <= 2) vspan(c, nx + dx, gy - 6, gy + gh + 6, cc);
        else if ((unsigned)(nx + dx) < (unsigned)c->w)
            for (int y = gy - 6; y <= gy + gh + 6; y++) {
                uint32_t *p = c->px + (size_t)(nx + dx) * c->h + y;
                *p = mix(*p, cc, 0.5f * (float)(9 - a) / 7.0f);
            }
    }

    snprintf(buf, sizeof buf, "A4=%dHZ", d->ref);
    sd_text(c, c->w - 12 - sd_text_width(buf, 2), 12, 2, C_TICK, buf);
}

/* ---- strobe view ---- */

static void draw_strobe(tuner_t *d, scr_canvas_t *c, int dim)
{
    uint32_t cc = dim ? C_DIM : cent_color(d->smooth_cents);
    char buf[24];

    note_name(d, buf, sizeof buf);
    glow_text(c, c->w / 2 - sd_text_width(buf, 6) / 2, 10, 6, dim ? C_DIM : C_PINK, dim ? C_SKY_HZN : C_CYAN, buf);
    snprintf(buf, sizeof buf, "%+d", (int)roundf(d->smooth_cents));
    glow_text(c, c->w / 2 - sd_text_width(buf, 3) / 2, 62, 3, cc, dim ? C_SKY_HZN : C_PURPLE, buf);

    int sy = 100, sh = horizon(c) - 8 - sy;
    static const int periods[] = { 32, 48, 64, 96 };
    static const uint32_t cols[] = { C_PINK, C_MAGENTA, C_PURPLE, C_CYAN };
    int nb = 4, bh = sh / nb;

    if (!dim) d->strobe_off += d->smooth_cents * 0.3f;
    if (d->strobe_off > 1e5f || d->strobe_off < -1e5f) d->strobe_off = 0;
    int off = (int)d->strobe_off;
    for (int b = 0; b < nb; b++) {
        int by = sy + b * bh, per = periods[b];
        uint32_t col = dim ? C_DIM : cols[b];
        for (int x = 0; x < c->w; x++) {
            int ph = ((x + off) % per + per) % per;
            if (ph < per / 2) vspan(c, x, by, by + bh - 5, col);
        }
    }
    vspan(c, c->w / 2, sy - 4, sy + sh, cc);
    vspan(c, c->w / 2 + 1, sy - 4, sy + sh, cc);

    snprintf(buf, sizeof buf, "A4=%dHZ", d->ref);
    sd_text(c, c->w - 12 - sd_text_width(buf, 2), 12, 2, C_TICK, buf);
}

/* ---- main draw callback ---- */

static void draw_view(tuner_t *d, scr_canvas_t *c, int dim)
{
    if (d->view == VIEW_NEEDLE) draw_needle(d, c, dim);
    else draw_strobe(d, c, dim);
}

static void idle(scr_canvas_t *c, const char *s)
{
    glow_text(c, c->w / 2 - sd_text_width(s, 8) / 2, c->h / 3 - 28, 8, C_DIM, C_SKY_TOP, s);
}

static int draw(void *owner, scr_canvas_t *c)
{
    tuner_t *d = owner;
    unsigned w = __atomic_load_n(&d->wpos, __ATOMIC_ACQUIRE);

    if (w < NFULL) {
        clear(d, c, 0);
        idle(c, "WAITING");
        return 1;
    }

    unsigned start = w - NFULL;
    for (int i = 0; i < NFULL; i++)
        d->x[i] = d->ring[(start + (unsigned)i) % RING] / 32768.0f;

    float rms = 0;
    for (int i = 0; i < NFULL; i++) rms += d->x[i] * d->x[i];
    rms = sqrtf(rms / NFULL);
    char lbuf[24];
    snprintf(lbuf, sizeof lbuf, "IN %.0f DB", rms > 1e-6f ? 20.0 * log10((double)rms) : -120.0);

    if (rms < GATE) {
        clear(d, c, 0);
        if (d->hold_frames > 0) {
            d->hold_frames--;
            draw_view(d, c, 1);
        } else {
            d->has = 0;
            idle(c, "---");
        }
        sd_text(c, 12, 12, 2, C_DIM, lbuf);
        return 1;
    }

    for (int i = 0; i < NDEC; i++) {
        float sum = 0;
        for (int j = 0; j < DEC; j++) sum += d->x[i * DEC + j];
        d->xd[i] = sum / DEC;
    }

    float lag = yin_lag(d);
    float hz = lag > 0 ? 44100.0f / refine(d, lag) : 0;

    if (hz > 20.0f && hz < 5000.0f) {
        d->med[d->nmed % NMED] = hz;
        d->nmed++;
        int n = d->nmed < NMED ? d->nmed : NMED;
        float sorted[NMED];
        for (int i = 0; i < n; i++) sorted[i] = d->med[(d->nmed - n + i) % NMED];
        for (int i = 0; i < n - 1; i++)
            for (int j = i + 1; j < n; j++)
                if (sorted[j] < sorted[i]) { float tmp = sorted[i]; sorted[i] = sorted[j]; sorted[j] = tmp; }
        hz = sorted[n / 2];

        float exact = 69.0f + 12.0f * log2f(hz / (float)d->ref);
        int midi = (int)roundf(exact);
        float cents = (exact - (float)midi) * 100.0f;

        d->hz = hz;
        d->midi = midi;
        d->cents = cents;
        d->has = 1;
        d->hold_frames = (int)(HOLD_S * (c->fps > 0 ? c->fps : 30));
        d->smooth_cents = d->smooth_cents * 0.7f + cents * 0.3f;
    }

    clear(d, c, d->has);
    if (d->has) draw_view(d, c, 0);
    else idle(c, "---");
    sd_text(c, 12, 12, 2, C_TICK, lbuf);
    return 1;
}

/* ---- engine ---- */

static void *create(const char *dir)
{
    (void)dir;
    tuner_t *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->ref = 440;
    scr_attach(d, draw);
    return d;
}

static void destroy(void *i)
{
    tuner_t *d = i;
    scr_detach(d);
    free(d->bg);
    free(d);
}

static void e_midi(void *i, const uint8_t *m, int n) { (void)i; (void)m; (void)n; }

static void set_param(void *i, const char *k, const char *v)
{
    tuner_t *d = i;
    if (!strcmp(k, "ref")) d->ref = atoi(v);
    else if (!strcmp(k, "mute")) d->mute = atoi(v) != 0;
    else if (!strcmp(k, "view")) { int m = atoi(v); d->view = m < 0 ? 0 : m > VIEW_STROBE ? VIEW_STROBE : m; }
    else if (!strcmp(k, "target")) { int t = atoi(v); d->target = t < 0 ? 0 : t > 2 ? 2 : t; scr_set_target(d->target); }
}

static int get_param(void *i, const char *k, char *b, int n)
{
    tuner_t *d = i;
    if (!strcmp(k, "status")) return scr_status(b, n);
    if (!strcmp(k, "ref")) return snprintf(b, n, "%d", d->ref);
    if (!strcmp(k, "mute")) return snprintf(b, n, "%d", d->mute);
    if (!strcmp(k, "view")) return snprintf(b, n, "%d", d->view);
    if (!strcmp(k, "target")) return snprintf(b, n, "%d", d->target);
    return 0;
}

static void render(void *i, int16_t *o, int f) { (void)i; memset(o, 0, (size_t)f * 4); }

static void process(void *i, const int16_t *in, int16_t *out, int frames)
{
    tuner_t *d = i;
    if (d->mute)
        memset(out, 0, (size_t)frames * 4);
    else
        memcpy(out, in, (size_t)frames * 4);
    unsigned w = d->wpos;
    for (int k = 0; k < frames; k++, w++)
        d->ring[w % RING] = (int16_t)(((int)in[k * 2] + (int)in[k * 2 + 1]) / 2);
    __atomic_store_n(&d->wpos, w, __ATOMIC_RELEASE);
}

static const mpc_engine_t ENGINE = { create, destroy, e_midi, set_param, get_param, render, process, NULL };
const mpc_engine_t *mpc_engine(void) { return &ENGINE; }
