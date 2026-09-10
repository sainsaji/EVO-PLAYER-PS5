/*
 * evo_keyboard.c — Global keyboard modal implementation.
 * Supports both Native PlayStation 5 IME Dialog and Custom Virtual Keyboard.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "evo_keyboard.h"
#include "evo_ime_dialog.h"
#include "evo_feedback.h"
#include "evo_boot_log.h"       /* #34: every native-IME step lands in evo.log */
#include "evo_boot_trace.h"     /* evo_bt: the pre-unjail probe also goes to klog */
#include "evo_rmlui_bridge.h"   /* #81: the modal is an RmlUi document now */

#define KB_MAX_BUF 256

/*
 * #34: the native PS5 IME is back on the app module.
 *
 * It used to be pinned off there: libSceImeDialog's symbols were reachable only
 * through the SDK stub, libSceCommonDialog was not linked at all, and the code
 * tried to sceKernelLoadStartModule it at open time - which a fake-signed module
 * cannot do. Now libSceCommonDialog is a positional PRX import
 * (tools/native-app/stubs/prx, scripts/package-app.sh step 6b) exactly like
 * libSceVideodec2, and sceCommonDialogInitialize is called directly.
 *
 * The dialog is still Sony code reached from a fake-signed process, so nothing
 * here assumes it works: every native open is checked, and the FIRST failure
 * latches g_native_ime_broken for the rest of the session so the virtual
 * keyboard takes over silently instead of failing once per keystroke-prompt.
 * Settings -> Interface -> Keyboard Input still chooses, and the choice
 * persists.
 */
static int g_kb_type = EVO_KEYBOARD_TYPE_NATIVE;
static int g_native_ime_broken = 0;   /* latched after a failed native open */

void evo_keyboard_set_type(int type)
{
    g_kb_type = (type == EVO_KEYBOARD_TYPE_VIRTUAL) ? EVO_KEYBOARD_TYPE_VIRTUAL : EVO_KEYBOARD_TYPE_NATIVE;
}

int evo_keyboard_get_type(void)
{
    return g_kb_type;
}

#if defined(EVO_TARGET_PS5) || defined(__FreeBSD__)
static uint16_t g_ime_buf[1024];
static uint16_t g_ime_title[256];
static uint16_t g_ime_placeholder[256];

void toast(const char *title, const char *msg);

/* 0 once the IME subsystem is up and a dialog may be opened. */
static int s_ime_attempted = 0;
static int s_ime_result = -1;

/*
 * Bring the CommonDialog family up once per process.
 *
 * Returns 0 when the IME can be opened, negative when it cannot. A second
 * initialise returns SCE_COMMON_DIALOG_ERROR_ALREADY_INITIALIZED (0x80B80002),
 * which is a success as far as we are concerned - hence the cached result.
 */
static int init_native_ime_subsystem(void)
{
    if (s_ime_attempted) return s_ime_result;
    s_ime_attempted = 1;

#if defined(EVO_APP_MODULE)
    /*
     * App module: this only ever runs from evo_keyboard_ime_probe(), in main()'s
     * pre-unjail slot. Both halves have to happen there.
     *
     * sceSysmoduleLoadModule is the half that #34 was missing. libSceImeDialog
     * links, its .sprx is NEEDED, and sceCommonDialogInitialize() returns 0 -
     * and then the first sceImeDialog* call faults, because the dialog's own
     * loadable module was never brought in. Same pre-unjail rule as
     * libSceVideodec2 (#31): after evo_jailbreak_self() swaps credentials
     * sceSysmoduleLoadModule stops working, so loading it lazily on the first
     * keyboard-open - which is what the old code did - cannot work either.
     */
    int sm = sceSysmoduleLoadModule(SCE_SYSMODULE_IME_DIALOG);
    evo_bt("ime: sceSysmoduleLoadModule(0x96) -> 0x%08x", (unsigned)sm);
    if (sm < 0) {
        s_ime_result = sm;
        return s_ime_result;
    }

    /* Direct import - libSceCommonDialog.sprx is a positional DT_NEEDED here. */
    int rc = sceCommonDialogInitialize();
    evo_bt("ime: sceCommonDialogInitialize() -> 0x%08x", (unsigned)rc);
#else
    /* Payload / host builds: no such import, so go the long way round. */
    int rc = -1;
    int res = 0;
    int mod = sceKernelLoadStartModule("libSceCommonDialog.sprx", 0, NULL, 0, NULL, &res);
    if (mod <= 0)
        mod = sceKernelLoadStartModule("/system/common/lib/libSceCommonDialog.sprx", 0, NULL, 0, NULL, &res);
    printf("[evo-ime] libSceCommonDialog handle=%d res=%d\n", mod, res);
    if (mod > 0) {
        int (*p_cmn_init)(void) = NULL;
        if (sceKernelDlsym(mod, "sceCommonDialogInitialize", (void**)&p_cmn_init) == 0 && p_cmn_init)
            rc = p_cmn_init();
        else if (sceKernelDlsym(mod, "uoUpLGNkygk", (void**)&p_cmn_init) == 0 && p_cmn_init)
            rc = p_cmn_init();
        printf("[evo-ime] sceCommonDialogInitialize() => 0x%08x (%d)\n", (unsigned)rc, rc);
    }
#endif

    /* 0x80B80002 = ALREADY_INITIALIZED: someone (or a previous open) got there
     * first, which is exactly the state we want to be in. */
    s_ime_result = (rc == 0 || (unsigned)rc == 0x80B80002u) ? 0 : (rc ? rc : -1);
    return s_ime_result;
}
#endif /* EVO_TARGET_PS5 || __FreeBSD__ */

/* See evo_keyboard.h — must run in main()'s pre-unjail slot. */
void evo_keyboard_ime_probe(void)
{
#if defined(EVO_APP_MODULE) && (defined(EVO_TARGET_PS5) || defined(__FreeBSD__))
    if (init_native_ime_subsystem() != 0) {
        g_native_ime_broken = 1;
        evo_bt("ime: native IME unavailable - virtual keyboard for this session");
    } else {
        evo_bt("ime: native IME ready");
    }
#endif
}

#if defined(EVO_TARGET_PS5) || defined(__FreeBSD__)

static void utf8_to_utf16(const char *src, uint16_t *dst, size_t max_dst)
{
    if (!dst || max_dst == 0) return;
    if (!src) {
        dst[0] = 0;
        return;
    }
    size_t d = 0;
    while (*src && d < max_dst - 1) {
        uint32_t codepoint = 0;
        unsigned char c = (unsigned char)*src;
        if (c < 0x80) {
            codepoint = c;
            src++;
        } else if ((c & 0xE0) == 0xC0) {
            codepoint = (c & 0x1F) << 6;
            if ((src[1] & 0xC0) == 0x80) {
                codepoint |= (src[1] & 0x3F);
                src += 2;
            } else { src++; continue; }
        } else if ((c & 0xF0) == 0xE0) {
            codepoint = (c & 0x0F) << 12;
            if ((src[1] & 0xC0) == 0x80 && (src[2] & 0xC0) == 0x80) {
                codepoint |= ((src[1] & 0x3F) << 6) | (src[2] & 0x3F);
                src += 3;
            } else { src++; continue; }
        } else if ((c & 0xF8) == 0xF0) {
            codepoint = (c & 0x07) << 18;
            if ((src[1] & 0xC0) == 0x80 && (src[2] & 0xC0) == 0x80 && (src[3] & 0xC0) == 0x80) {
                codepoint |= ((src[1] & 0x3F) << 12) | ((src[2] & 0x3F) << 6) | (src[3] & 0x3F);
                src += 4;
            } else { src++; continue; }
        } else {
            src++;
            continue;
        }

        if (codepoint < 0x10000) {
            dst[d++] = (uint16_t)codepoint;
        } else if (codepoint <= 0x10FFFF) {
            if (d + 1 < max_dst - 1) {
                codepoint -= 0x10000;
                dst[d++] = (uint16_t)(0xD800 + (codepoint >> 10));
                dst[d++] = (uint16_t)(0xDC00 + (codepoint & 0x3FF));
            } else {
                break;
            }
        }
    }
    dst[d] = 0;
}

static void utf16_to_utf8(const uint16_t *src, char *dst, size_t max_dst)
{
    if (!dst || max_dst == 0) return;
    if (!src) {
        dst[0] = 0;
        return;
    }
    size_t d = 0;
    while (*src && d < max_dst - 1) {
        uint32_t cp = *src++;
        if (cp >= 0xD800 && cp <= 0xDBFF && *src >= 0xDC00 && *src <= 0xDFFF) {
            cp = 0x10000 + (((cp & 0x3FF) << 10) | (*src++ & 0x3FF));
        }

        if (cp < 0x80) {
            if (d + 1 < max_dst) dst[d++] = (char)cp;
        } else if (cp < 0x800) {
            if (d + 2 < max_dst) {
                dst[d++] = (char)(0xC0 | (cp >> 6));
                dst[d++] = (char)(0x80 | (cp & 0x3F));
            }
        } else if (cp < 0x10000) {
            if (d + 3 < max_dst) {
                dst[d++] = (char)(0xE0 | (cp >> 12));
                dst[d++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                dst[d++] = (char)(0x80 | (cp & 0x3F));
            }
        } else if (cp <= 0x10FFFF) {
            if (d + 4 < max_dst) {
                dst[d++] = (char)(0xF0 | (cp >> 18));
                dst[d++] = (char)(0x80 | ((cp >> 12) & 0x3F));
                dst[d++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                dst[d++] = (char)(0x80 | (cp & 0x3F));
            }
        }
    }
    dst[d] = 0;
}
#endif


/* Key grids for Virtual Keyboard */
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
    int             is_native_active;
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

#if defined(EVO_TARGET_PS5) || defined(__FreeBSD__)
    if (g_kb_type == EVO_KEYBOARD_TYPE_NATIVE && !g_native_ime_broken) {
        int cd_rc = init_native_ime_subsystem();
        if (cd_rc != 0) {
            /* No CommonDialog, no IME. Don't try again this session. */
            g_native_ime_broken = 1;
            evo_log("ime: CommonDialog unavailable (0x%08x) - virtual keyboard",
                    (unsigned)cd_rc);
            evo_log_flush();
            toast("KEYBOARD", "USING VIRTUAL KEYBOARD");
            goto virtual_keyboard;
        }

        utf8_to_utf16(g_kb.title, g_ime_title, 256);
        utf8_to_utf16(initial_value ? initial_value : "", g_ime_buf, 1024);
        utf8_to_utf16("Enter text...", g_ime_placeholder, 256);

int prospero_get_initial_user_id(void);

        int userId = prospero_get_initial_user_id();
        if (userId <= 0) {
            if (sceUserServiceGetInitialUser(&userId) < 0 || userId <= 0) {
                int login_users[4] = {0};
                if (sceUserServiceGetLoginUserIdList(login_users) >= 0 && login_users[0] > 0) {
                    userId = login_users[0];
                } else {
                    userId = 0x10000000;
                }
            }
        }


        SceImeDialogParam param;
        memset(&param, 0, sizeof(param));
        param.userId = userId;
        param.type = SCE_IME_TYPE_DEFAULT;
        param.enterLabel = SCE_IME_ENTER_LABEL_DEFAULT;
        param.inputMethod = SCE_IME_INPUT_METHOD_DEFAULT;
        param.option = SCE_IME_OPTION_NONE;
        param.maxTextLength = (uint32_t)g_kb.max_len;
        param.inputTextBuffer = g_ime_buf;
        param.title = g_ime_title;
        param.placeholder = g_ime_placeholder;

        /* One breadcrumb per system call: a fault inside libSceImeDialog kills
         * the process outright, so the last line in evo.log is the only way to
         * know which call did it. This is how #34's second failure was found. */
        uint32_t width = 0, height = 0;
        evo_log("ime: -> sceImeDialogGetPanelSizeExtended");
        evo_log_flush();
        if (sceImeDialogGetPanelSizeExtended(&param, NULL, &width, &height) >= 0 && width > 0 && height > 0) {
            param.posx = (1920.0f - (float)width) / 2.0f;
            param.posy = (1080.0f - (float)height) / 2.0f;
            param.horizontalAlignment = SCE_IME_HALIGN_LEFT;
            param.verticalAlignment = SCE_IME_VALIGN_TOP;
        }

        evo_log("ime: -> sceImeDialogInit (panel %ux%u)",
                (unsigned)width, (unsigned)height);
        evo_log_flush();
        int rc = sceImeDialogInit(&param, NULL);
        evo_log("ime: sceImeDialogInit userId=0x%08x len=%u panel=%ux%u -> 0x%08x",
                (unsigned)userId, (unsigned)param.maxTextLength,
                (unsigned)width, (unsigned)height, (unsigned)rc);
        evo_log_flush();
        printf("[evo-ime] sceImeDialogInit userId=0x%08x len=%u rc=0x%08x (%d)\n",
               (unsigned)userId, (unsigned)param.maxTextLength, (unsigned)rc, rc);
        fflush(stdout);

        if (rc >= 0) {
            g_kb.is_native_active = 1;
            evo_feedback(EVO_FB_OPEN);
            return;
        }

        /* Latch: if the system dialog won't start once it won't start later
         * either, and a failed toast per prompt is worse than just typing. */
        g_native_ime_broken = 1;
        char err_msg[64];
        snprintf(err_msg, sizeof(err_msg), "ERR 0x%08X", (unsigned)rc);
        toast("NATIVE IME FALLBACK", err_msg);
    }
virtual_keyboard:   /* both native bail-outs land here */
#endif


    g_kb.is_native_active = 0;
    evo_feedback(EVO_FB_OPEN);
}

void evo_keyboard_close(void)
{
#if defined(EVO_TARGET_PS5) || defined(__FreeBSD__)
    if (g_kb.is_open && g_kb.is_native_active) {
        sceImeDialogAbort();
        sceImeDialogTerm();
        g_kb.is_native_active = 0;
    }
#endif
    g_kb.is_open = 0;
}

int evo_keyboard_is_open(void)
{
    return g_kb.is_open;
}

int evo_keyboard_is_native_active(void)
{
    return g_kb.is_open && g_kb.is_native_active;
}

const char *evo_keyboard_get_text(void)
{
    return g_kb.buffer;
}

void evo_keyboard_update(void)
{
#if defined(EVO_TARGET_PS5) || defined(__FreeBSD__)
    if (g_kb.is_open && g_kb.is_native_active) {
        int status = sceImeDialogGetStatus();
        if (status == SCE_IME_DIALOG_STATUS_FINISHED) {
            SceImeDialogResult result;
            memset(&result, 0, sizeof(result));
            sceImeDialogGetResult(&result);
            if (result.endStatus == SCE_IME_DIALOG_END_STATUS_OK) {
                utf16_to_utf8(g_ime_buf, g_kb.buffer, sizeof(g_kb.buffer));
                if (g_kb.callback) {
                    g_kb.callback(g_kb.buffer, g_kb.userdata);
                }
                evo_feedback(EVO_FB_CONFIRM);
            } else {
                evo_feedback(EVO_FB_CANCEL);
            }
            sceImeDialogTerm();
            g_kb.is_open = 0;
            g_kb.is_native_active = 0;
        } else if (status == SCE_IME_DIALOG_STATUS_NONE) {
            sceImeDialogTerm();
            g_kb.is_open = 0;
            g_kb.is_native_active = 0;
        }
    }
#endif
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

    if (g_kb.is_native_active) {
        evo_keyboard_update();
        return 1; /* Consume all pad events while native IME is active */
    }

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

/*
 * #81: the keyboard modal is an RmlUi document (assets/rml/keyboard.rml) in its
 * own Rml::Context now — this pushes g_kb's state (buffer / layer / D-pad focus)
 * to the bridge and composites it over the framebuffer, exactly where the old
 * immediate-mode renderer used to draw. All the state + input handling above is
 * unchanged. Closes #34 (the native IME path stays only as an opt-in for
 * non-app builds; the app module defaults to virtual).
 */
void evo_screen_keyboard(uint32_t *fb)
{
    evo_keyboard_params_t p;
    memset(&p, 0, sizeof(p));

    if (!g_kb.is_open) {
        evo_rmlui_update_keyboard(&p);   /* visible = 0 -> hide the doc */
        return;
    }

    p.visible     = 1;
    p.native_only = g_kb.is_native_active;
    p.title       = g_kb.title;
    p.text        = g_kb.buffer;
    p.mode_label  = (g_kb.mode == 0) ? "LOWERCASE"
                  : (g_kb.mode == 1) ? "UPPERCASE" : "SYMBOLS";
    {
        const char *const *grid = (g_kb.mode == 0) ? GRID_LOWER
                                : (g_kb.mode == 1) ? GRID_UPPER : GRID_SYMBOLS;
        for (int i = 0; i < 4; i++) p.rows[i] = grid[i];
    }
    for (int i = 0; i < KB_ACT_COUNT; i++) p.action_labels[i] = ACTION_LABELS[i];
    p.len       = (int)strlen(g_kb.buffer);
    p.max_len   = g_kb.max_len;
    p.focus_row = g_kb.row;
    p.focus_col = g_kb.col;

    g_kb.blink_timer = (g_kb.blink_timer + 1) % 60;
    p.show_caret = (g_kb.blink_timer < 36);

    evo_rmlui_update_keyboard(&p);
    evo_rmlui_render_keyboard(fb, 1920, 1080);
}

