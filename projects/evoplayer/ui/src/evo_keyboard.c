/*
 * evo_keyboard.c — Global on-screen virtual keyboard modal implementation.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "evo_keyboard.h"
#include "evo_theme.h"
#include "evo_ui.h"
#include "evo_feedback.h"
#include "evo_draw.h"
#include "evo_widgets.h"

#define KB_MAX_BUF 256

/* Key grids */
static const char *const GRID_LOWER[4] = {
    "1234567890",
    "qwertyuiop",
    "asdfghjkl.",
    "zxcvbnm_-:"
};

static const char *const GRID_UPPER[4] = {
    "1234567890",
    "QWERTYUIOP",
    "ASDFGHJKL.",
    "ZXCVBNM_-:"
};

static const char *const GRID_SYMBOLS[4] = {
    "!@#$%^&*()",
    "~`+=[]{}\\|",
    ";:\"'<>/?,.",
    "_-@/.:%=+*"
};

/* Actions on Row 4 */
enum {
    KB_ACT_MODE = 0,
    KB_ACT_SPACE,
    KB_ACT_BACKSPACE,
    KB_ACT_CLEAR,
    KB_ACT_CANCEL,
    KB_ACT_DONE,
    KB_ACT_COUNT
};

static const char *const ACTION_LABELS[KB_ACT_COUNT] = {
    "123 / ABC",
    "SPACE",
    "BACKSPACE",
    "CLEAR",
    "CANCEL",
    "DONE"
};

typedef struct {
    int             is_open;
    char            title[128];
    char            buffer[KB_MAX_BUF];
    int             max_len;
    int             mode;        /* 0 = lower, 1 = upper, 2 = symbols */
    int             row;         /* 0..4 */
    int             col;         /* 0..9 for rows 0..3; 0..5 for row 4 */
    uint32_t        blink_timer;
    evo_keyboard_cb callback;
    void           *userdata;
} evo_keyboard_state_t;

static evo_keyboard_state_t g_kb;

void evo_keyboard_open(const char *title,
                       const char *initial_value,
                       int max_len,
                       evo_keyboard_cb on_submit,
                       void *userdata)
{
    memset(&g_kb, 0, sizeof(g_kb));
    g_kb.is_open = 1;
    if (title) strncpy(g_kb.title, title, sizeof(g_kb.title) - 1);
    else strncpy(g_kb.title, "ENTER TEXT", sizeof(g_kb.title) - 1);

    if (initial_value) {
        strncpy(g_kb.buffer, initial_value, sizeof(g_kb.buffer) - 1);
    }

    g_kb.max_len = (max_len > 0 && max_len < KB_MAX_BUF) ? max_len : (KB_MAX_BUF - 1);
    g_kb.mode = 0;
    g_kb.row = 1;    /* Start on 'q' / first letter row */
    g_kb.col = 0;
    g_kb.blink_timer = 0;
    g_kb.callback = on_submit;
    g_kb.userdata = userdata;

    evo_feedback(EVO_FB_OPEN);
}

void evo_keyboard_close(void)
{
    g_kb.is_open = 0;
}

int evo_keyboard_is_open(void)
{
    return g_kb.is_open;
}

const char *evo_keyboard_get_text(void)
{
    return g_kb.buffer;
}

static void insert_char(char c)
{
    size_t len = strlen(g_kb.buffer);
    if ((int)len < g_kb.max_len && len + 1 < sizeof(g_kb.buffer)) {
        g_kb.buffer[len] = c;
        g_kb.buffer[len + 1] = '\0';
        evo_feedback(EVO_FB_MOVE);
    } else {
        evo_feedback(EVO_FB_BOUNDARY);
    }
}

static void backspace_char(void)
{
    size_t len = strlen(g_kb.buffer);
    if (len > 0) {
        g_kb.buffer[len - 1] = '\0';
        evo_feedback(EVO_FB_TOGGLE);
    } else {
        evo_feedback(EVO_FB_BOUNDARY);
    }
}

static void submit_text(void)
{
    if (g_kb.callback) {
        g_kb.callback(g_kb.buffer, g_kb.userdata);
    }
    evo_feedback(EVO_FB_CONFIRM);
    g_kb.is_open = 0;
}

#define EVO_PAD_UP        0x0010
#define EVO_PAD_RIGHT     0x0020
#define EVO_PAD_DOWN      0x0040
#define EVO_PAD_LEFT      0x0080
#define EVO_PAD_L1        0x0400
#define EVO_PAD_R1        0x0800
#define EVO_PAD_TRIANGLE  0x1000
#define EVO_PAD_CIRCLE    0x2000
#define EVO_PAD_CROSS     0x4000
#define EVO_PAD_SQUARE    0x8000

int evo_keyboard_handle_input(uint32_t pressed)
{
    if (!g_kb.is_open) return 0;

    /* D-PAD Navigation */
    if (pressed & EVO_PAD_UP) {
        if (g_kb.row > 0) {
            g_kb.row--;
            if (g_kb.row < 4 && g_kb.col >= 10) g_kb.col = 9;
        } else {
            g_kb.row = 4;
            if (g_kb.col >= KB_ACT_COUNT) g_kb.col = KB_ACT_COUNT - 1;
        }
        evo_feedback(EVO_FB_MOVE);
        return 1;
    }

    if (pressed & EVO_PAD_RIGHT) {
        int max_c = (g_kb.row == 4) ? KB_ACT_COUNT : 10;
        g_kb.col = (g_kb.col + 1) % max_c;
        evo_feedback(EVO_FB_MOVE);
        return 1;
    }

    if (pressed & EVO_PAD_DOWN) {
        if (g_kb.row < 4) {
            g_kb.row++;
            if (g_kb.row == 4 && g_kb.col >= KB_ACT_COUNT) {
                g_kb.col = KB_ACT_COUNT - 1;
            }
        } else {
            g_kb.row = 0;
        }
        evo_feedback(EVO_FB_MOVE);
        return 1;
    }

    if (pressed & EVO_PAD_LEFT) {
        int max_c = (g_kb.row == 4) ? KB_ACT_COUNT : 10;
        g_kb.col = (g_kb.col + max_c - 1) % max_c;
        evo_feedback(EVO_FB_MOVE);
        return 1;
    }

    /* Shortcuts */
    if (pressed & EVO_PAD_SQUARE) { /* SQUARE — Quick Backspace */
        backspace_char();
        return 1;
    }

    if (pressed & EVO_PAD_TRIANGLE) { /* TRIANGLE — Quick Done */
        submit_text();
        return 1;
    }

    if (pressed & EVO_PAD_CIRCLE) { /* CIRCLE — Quick Cancel */
        evo_feedback(EVO_FB_CANCEL);
        g_kb.is_open = 0;
        return 1;
    }

    if (pressed & (EVO_PAD_L1 | EVO_PAD_R1)) { /* L1 or R1 — Cycle Mode */
        g_kb.mode = (g_kb.mode + 1) % 3;
        evo_feedback(EVO_FB_TOGGLE);
        return 1;
    }

    /* CROSS — Activate focused key */
    if (pressed & EVO_PAD_CROSS) {
        if (g_kb.row < 4) {
            const char *row_chars = (g_kb.mode == 0) ? GRID_LOWER[g_kb.row] :
                                    (g_kb.mode == 1) ? GRID_UPPER[g_kb.row] :
                                                       GRID_SYMBOLS[g_kb.row];
            if (g_kb.col < 10) {
                insert_char(row_chars[g_kb.col]);
            }
        } else {
            /* Action Bar */
            switch (g_kb.col) {
                case KB_ACT_MODE:
                    g_kb.mode = (g_kb.mode + 1) % 3;
                    evo_feedback(EVO_FB_TOGGLE);
                    break;
                case KB_ACT_SPACE:
                    insert_char(' ');
                    break;
                case KB_ACT_BACKSPACE:
                    backspace_char();
                    break;
                case KB_ACT_CLEAR:
                    g_kb.buffer[0] = '\0';
                    evo_feedback(EVO_FB_TOGGLE);
                    break;
                case KB_ACT_CANCEL:
                    evo_feedback(EVO_FB_CANCEL);
                    g_kb.is_open = 0;
                    break;
                case KB_ACT_DONE:
                    submit_text();
                    break;
            }
        }
        return 1;
    }

    return 0;
}

static uint32_t with_alpha(uint32_t bgra, uint8_t a)
{
    return (bgra & 0x00FFFFFFu) | ((uint32_t)a << 24);
}

void evo_screen_keyboard(uint32_t *fb)
{
    if (!g_kb.is_open) return;

    const evo_theme *th = evo_theme_current();

    /* 1. Scrim background (smooth dark dim over current screen) */
    evo_ui_vgrad_over(fb, 0, 0, EVO_UI_W, EVO_UI_H,
                      with_alpha(th->bg_bottom, 220),
                      with_alpha(th->bg_top, 240));

    /* 2. Main modal card panel */
    int panel_x = 260;
    int panel_y = 140;
    int panel_w = 1400;
    int panel_h = 800;
    int radius  = 20;

    evo_ui_round_rect(fb, panel_x, panel_y, panel_w, panel_h, radius,
                      with_alpha(th->surface, 250),
                      with_alpha(th->surface_sel, 250),
                      with_alpha(th->border, 180), 2,
                      with_alpha(th->shadow, 140), 24);

    /* 3. Title & Header Hints */
    int title_y = panel_y + 36;
    evo_text(fb, panel_x + 50, title_y, g_kb.title, th->text_primary, EVO_FACE_TITLE);

    /* Mode indicator tag */
    const char *mode_str = (g_kb.mode == 0) ? "LOWERCASE" :
                           (g_kb.mode == 1) ? "UPPERCASE" : "SYMBOLS";
    evo_text(fb, panel_x + panel_w - 200, title_y + 4, mode_str, th->accent, EVO_FACE_SUB);

    /* 4. Text Input Field Box */
    int input_x = panel_x + 50;
    int input_y = panel_y + 96;
    int input_w = panel_w - 100;
    int input_h = 76;

    evo_ui_round_rect(fb, input_x, input_y, input_w, input_h, 12,
                      with_alpha(th->bg_top, 240),
                      with_alpha(th->bg_bottom, 240),
                      with_alpha(th->accent, 160), 2,
                      with_alpha(th->shadow, 80), 8);

    /* Text display */
    char disp_buf[KB_MAX_BUF + 2];
    snprintf(disp_buf, sizeof(disp_buf), "%s", g_kb.buffer);

    g_kb.blink_timer = (g_kb.blink_timer + 1) % 60;
    int show_cursor = (g_kb.blink_timer < 36);

    int text_x = input_x + 24;
    int text_y = input_y + 20;

    if (g_kb.buffer[0]) {
        evo_text(fb, text_x, text_y, disp_buf, th->text_primary, EVO_FACE_MENU);
        int tw = evo_text_w(disp_buf, EVO_FACE_MENU);
        if (show_cursor) {
            evo_ui_round_rect(fb, text_x + tw + 4, text_y - 2, 4, 34, 2,
                              th->accent, th->accent, 0, 0, 0, 0);
        }
    } else {
        evo_text(fb, text_x, text_y, "Type here using controller or connected keyboard...",
                 with_alpha(th->text_muted, 120), EVO_FACE_MENU);
        if (show_cursor) {
            evo_ui_round_rect(fb, text_x, text_y - 2, 4, 34, 2,
                              th->accent, th->accent, 0, 0, 0, 0);
        }
    }

    /* Counter indicator */
    char count_str[32];
    snprintf(count_str, sizeof(count_str), "%d / %d", (int)strlen(g_kb.buffer), g_kb.max_len);
    evo_text(fb, input_x + input_w - 120, text_y + 4, count_str, th->text_muted, EVO_FACE_SUB);

    /* 5. Key Grid Rows 0..3 */
    int grid_start_y = input_y + input_h + 30;
    int key_w = 118;
    int key_h = 68;
    int gap_x = 12;
    int gap_y = 12;
    int grid_start_x = panel_x + 55;

    for (int r = 0; r < 4; r++) {
        const char *chars = (g_kb.mode == 0) ? GRID_LOWER[r] :
                            (g_kb.mode == 1) ? GRID_UPPER[r] : GRID_SYMBOLS[r];
        int ky = grid_start_y + r * (key_h + gap_y);

        for (int c = 0; c < 10; c++) {
            int kx = grid_start_x + c * (key_w + gap_x);
            int is_focused = (g_kb.row == r && g_kb.col == c);

            uint32_t fill_top = is_focused ? th->surface_sel : th->surface;
            uint32_t fill_bot = is_focused ? th->accent : th->bg_top;
            uint32_t border   = is_focused ? th->accent : with_alpha(th->border, 100);
            int border_px     = is_focused ? 3 : 1;

            evo_ui_round_rect(fb, kx, ky, key_w, key_h, 10,
                              fill_top, fill_bot, border, border_px,
                              is_focused ? with_alpha(th->accent, 100) : 0, is_focused ? 10 : 0);

            char key_str[2] = { chars[c], '\0' };
            int kw = evo_text_w(key_str, EVO_FACE_MENU);
            int tx = kx + (key_w - kw) / 2;
            int ty = ky + (key_h - 28) / 2;

            uint32_t text_col = is_focused ? 0xFFFFFFFFu : th->text_primary;
            evo_text(fb, tx, ty, key_str, text_col, EVO_FACE_MENU);
        }
    }

    /* 6. Action Bar Row 4 */
    int action_y = grid_start_y + 4 * (key_h + gap_y);
    int act_widths[KB_ACT_COUNT] = { 180, 360, 200, 160, 160, 180 };
    int cur_act_x = grid_start_x;

    for (int a = 0; a < KB_ACT_COUNT; a++) {
        int aw = act_widths[a];
        int is_focused = (g_kb.row == 4 && g_kb.col == a);

        uint32_t fill_top = is_focused ? th->surface_sel : th->surface;
        uint32_t fill_bot = is_focused ? th->accent : th->bg_top;
        uint32_t border   = is_focused ? th->accent : with_alpha(th->border, 100);
        int border_px     = is_focused ? 3 : 1;

        if (a == KB_ACT_DONE && is_focused) {
            fill_bot = 0xFF24A024u; /* Vibrant green hint for Done */
        }

        evo_ui_round_rect(fb, cur_act_x, action_y, aw, key_h, 10,
                          fill_top, fill_bot, border, border_px,
                          is_focused ? with_alpha(th->accent, 100) : 0, is_focused ? 10 : 0);

        int tw = evo_text_w(ACTION_LABELS[a], EVO_FACE_SUB);
        int tx = cur_act_x + (aw - tw) / 2;
        int ty = action_y + (key_h - 20) / 2;

        uint32_t text_col = is_focused ? 0xFFFFFFFFu : th->text_primary;
        evo_text(fb, tx, ty, ACTION_LABELS[a], text_col, EVO_FACE_SUB);

        cur_act_x += aw + gap_x;
    }

    /* 7. Bottom Controller Shortcut Hints */
    int hint_x = panel_x + 60;
    int hint_y = panel_y + panel_h - 38;

    struct { int glyph; const char *lbl; } hints[] = {
        { EVO_GLYPH_CROSS,    "SELECT" },
        { EVO_GLYPH_SQUARE,   "BACKSPACE" },
        { EVO_GLYPH_TRIANGLE, "DONE" },
        { EVO_GLYPH_CIRCLE,   "CANCEL" },
        { EVO_GLYPH_LSTICK,   "SHIFT (L1/R1)" }
    };
    int hint_count = 5;
    int hx = hint_x;
    for (int i = 0; i < hint_count; i++) {
        evo_glyph_tinted(fb, hx, hint_y - 20, hints[i].glyph, th->accent);
        evo_text(fb, hx + 52, hint_y - 8, hints[i].lbl, th->text_muted, EVO_FACE_SMALL);
        hx += 52 + evo_text_w(hints[i].lbl, EVO_FACE_SMALL) + 44;
    }
}
