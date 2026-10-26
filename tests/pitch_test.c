/* Offline pitch test: feeds the engine guitar- and bass-like tones (a fundamental plus decaying harmonics) at known
 * frequencies and runs its draw callback on a canvas of the skin's size, the way the drawing thread would, then checks
 * the note and the cents it settles on. No display: screendraw is stubbed out.
 *
 *   cc -fsanitize=address,undefined -g -Isrc -I<mpc-vst-plugins>/wrapper -o pitch_test tests/pitch_test.c -lm */
#include "../src/engine.c"

void scr_attach(void *owner, scr_draw_fn fn) { (void)owner; (void)fn; }
void scr_detach(void *owner) { (void)owner; }
void scr_set_fps(int fps) { (void)fps; }
void scr_set_target(int target) { (void)target; }
int scr_status(char *buf, int n) { return snprintf(buf, (size_t)n, "test"); }

static int fails;
static double ph;

static void feed(tuner_t *d, double hz, float amp, int frames)
{
    int16_t in[256], out[256];
    for (int done = 0; done < frames; done += 128) {
        for (int i = 0; i < 128; i++, ph += 2 * M_PI * hz / 44100) {
            double v = 0;
            for (int k = 1; k <= 8; k++) v += sin(ph * k) / (k * 1.3);
            in[i * 2] = in[i * 2 + 1] = (int16_t)(amp * 32767 * 0.55 * v);
        }
        process(d, in, out, 128);
    }
}

static void settle(tuner_t *d, scr_canvas_t *c, double hz, float amp)
{
    for (int f = 0; f < 40; f++) {                 /* 40 frames at 30 fps: 1470 samples between frames */
        feed(d, hz, amp, 1470);
        draw(d, c);
    }
}

static void check(tuner_t *d, scr_canvas_t *c, double hz, int ref, const char *want, float want_cents)
{
    d->ref = ref;
    settle(d, c, hz, 0.3f);
    char got[16];
    note_name(d, got, sizeof got);
    float err = d->smooth_cents - want_cents;
    int ok = d->has && !strcmp(got, want) && fabsf(err) < 0.5f;
    if (!ok) fails++;
    printf("%s %8.2f Hz (A4=%d): %-4s %+6.2f cents, want %-4s %+6.2f\n", ok ? "ok  " : "FAIL", hz, ref, d->has ? got : "-",
           (double)d->smooth_cents, want, (double)want_cents);
}

int main(void)
{
    tuner_t *d = create(NULL);
    scr_canvas_t c = { .w = 958, .h = 438, .fps = 30 };
    c.px = calloc((size_t)c.w * c.h, sizeof *c.px);
    if (!d || !c.px) return 1;

    static const struct { double hz; const char *note; } strings[] = {
        { 41.20, "E1" }, { 55.00, "A1" }, { 73.42, "D2" }, { 98.00, "G2" },               /* bass */
        { 82.41, "E2" }, { 110.00, "A2" }, { 146.83, "D3" }, { 196.00, "G3" }, { 246.94, "B3" }, { 329.63, "E4" },
        { 440.00, "A4" }, { 880.00, "A5" }, { 1046.50, "C6" },
    };
    for (size_t i = 0; i < sizeof strings / sizeof *strings; i++) check(d, &c, strings[i].hz, 440, strings[i].note, 0);
    check(d, &c, 110.0 * pow(2, 20 / 1200.0), 440, "A2", 20);    /* sharp and flat */
    check(d, &c, 82.41 * pow(2, -15 / 1200.0), 440, "E2", -15);
    check(d, &c, 432.0, 432, "A4", 0);                              /* another reference pitch */
    check(d, &c, 440.0, 432, "A4", 31.77f);

    for (int f = 0; f < 120; f++) { feed(d, 0, 0, 1470); draw(d, &c); }    /* silence: the note goes after the hold */
    if (d->has) { fails++; printf("FAIL note still shown 4 s into silence\n"); }
    else printf("ok   silence clears the note\n");

    int16_t in[256], out[256];                                      /* pass-through, then mute */
    for (int i = 0; i < 256; i++) in[i] = (int16_t)(i * 37);
    process(d, in, out, 128);
    if (memcmp(in, out, sizeof in)) { fails++; printf("FAIL audio not passed through\n"); }
    set_param(d, "mute", "1");
    process(d, in, out, 128);
    for (int i = 0; i < 256; i++) if (out[i]) { fails++; printf("FAIL mute let audio through\n"); break; }

    free(c.px);
    destroy(d);
    printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails != 0;
}
