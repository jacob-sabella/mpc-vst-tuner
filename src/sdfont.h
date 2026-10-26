/* A 5x7 bitmap font for screendraw canvases, drawn at any whole-number scale (a glyph is 6 * scale wide). */
#pragma once
#include <string.h>
#include "screendraw.h"

static const char SD_GLYPHS[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ.:/%-#+=";
static const uint8_t SD_FONT[][7] = {
    {14,17,19,21,25,17,14},{4,12,4,4,4,4,14},{14,17,1,2,4,8,31},{31,2,4,2,1,17,14},{2,6,10,18,31,2,2},
    {31,16,30,1,1,17,14},{6,8,16,30,17,17,14},{31,1,2,4,8,8,8},{14,17,17,14,17,17,14},{14,17,17,15,1,2,12},
    {14,17,17,31,17,17,17},{30,17,17,30,17,17,30},{14,17,16,16,16,17,14},{28,18,17,17,17,18,28},
    {31,16,16,30,16,16,31},{31,16,16,30,16,16,16},{14,17,16,23,17,17,15},{17,17,17,31,17,17,17},
    {14,4,4,4,4,4,14},{7,2,2,2,2,18,12},{17,18,20,24,20,18,17},{16,16,16,16,16,16,31},{17,27,21,21,17,17,17},
    {17,17,25,21,19,17,17},{14,17,17,17,17,17,14},{30,17,17,30,16,16,16},{14,17,17,17,21,18,13},
    {30,17,17,30,20,18,17},{15,16,16,14,1,1,30},{31,4,4,4,4,4,4},{17,17,17,17,17,17,14},{17,17,17,17,17,10,4},
    {17,17,17,21,21,21,10},{17,17,10,4,10,17,17},{17,17,17,10,4,4,4},{31,1,2,4,8,16,31},
    {0,0,0,0,0,12,12},{0,12,12,0,12,12,0},{0,1,2,4,8,16,0},{24,25,2,4,8,19,3},{0,0,0,31,0,0,0},
    {10,10,31,10,31,10,10},{0,4,4,31,4,4,0},{0,0,31,0,31,0,0},
};

static inline int sd_text_width(const char *s, int scale) { int n = (int)strlen(s); return n ? (6 * n - 1) * scale : 0; }

/* Text with its top left at (x, y); lower case is drawn as upper case, unknown characters as a space. */
static inline void sd_text(scr_canvas_t *c, int x, int y, int scale, uint32_t col, const char *s)
{
    for (; *s; s++, x += 6 * scale) {
        char ch = *s >= 'a' && *s <= 'z' ? (char)(*s - 32) : *s;
        const char *g = strchr(SD_GLYPHS, ch);
        if (!g) continue;
        const uint8_t *rows = SD_FONT[g - SD_GLYPHS];
        for (int r = 0; r < 7; r++)
            for (int b = 0; b < 5; b++)
                if (rows[r] >> (4 - b) & 1)
                    for (int dy = 0; dy < scale; dy++)
                        for (int dx = 0; dx < scale; dx++) scr_px(c, x + b * scale + dx, y + r * scale + dy, col);
    }
}
