/* screendraw: a plugin drawing its own pixels onto MPC's screen, beside the skin (proof of concept).
 *
 * One background thread per process draws into MPC's display buffers: into the buffer MPC draws its UI into (SOURCE,
 * found among the process's own mappings), or into the framebuffer on screen, followed through page flips (SCREEN).
 * screendraw.c says how each works.
 *
 * The skin reserves the area: a placed image (canvas.png, from make_canvas.py) whose left and right edge columns
 * carry two alternating colour patterns. The thread finds them on screen, so nothing about the header height,
 * the device or the rotation is hard-coded, and it draws a row only while both of that row's markers are on screen:
 * another page, a menu over the canvas or a different plugin hides them, and the thread leaves those rows alone.
 * It copies a frame when it has a new one, or when MPC has painted over the canvas (a few sampled columns no longer
 * hold what it drew), so a quiet screen costs a few small reads per tick.
 *
 * Instances attach and detach; the last one attached draws. The thread starts with the first and stops with the
 * last. The draw callback runs on that thread, never on MPC's audio or screen thread. */
#pragma once
#include <stdint.h>

/* Marker colours (0xRRGGBB). make_canvas.py reads these lines; keep the format. */
#define SCR_MARK_L_A 0x2EC4B6
#define SCR_MARK_L_B 0x0B0E14
#define SCR_MARK_R_A 0xC42E8C
#define SCR_MARK_R_B 0x0B0E14
#define SCR_CANVAS_BG 0x0B0E14
#define SCR_CANVAS_LINE 0x26303E

typedef struct {
    int w, h;            /* the drawable area, inside the markers */
    uint32_t *px;        /* column-major: pixel (x, y) is px[x * h + y], 0xRRGGBB */
    float fps;           /* frames drawn per second, measured */
    float blit_ms;       /* mean time of one copy to the screen */
    float draw_ms;       /* mean time of one draw callback */
    int repaints_s;      /* copies a second forced by MPC painting over the canvas */
    int flips_s;         /* page flips a second seen */
    int source;          /* drawing into MPC's own buffer (else the framebuffer on screen) */
} scr_canvas_t;

/* Draw the next frame into c. Return 0 if nothing changed (no copy is made). */
typedef int (*scr_draw_fn)(void *owner, scr_canvas_t *c);

void scr_attach(void *owner, scr_draw_fn fn);
void scr_detach(void *owner);          /* waits for a draw in progress; stops the thread after the last one */
void scr_set_fps(int fps);             /* 1..60, default 30 */
void scr_set_target(int target);       /* 0 auto (MPC's own buffer if the canvas is there), 1 screen, 2 MPC's buffer */
int  scr_status(char *buf, int n);     /* one line for a readout */

static inline void scr_px(scr_canvas_t *c, int x, int y, uint32_t col)
{
    if ((unsigned)x < (unsigned)c->w && (unsigned)y < (unsigned)c->h) c->px[x * c->h + y] = col;
}
