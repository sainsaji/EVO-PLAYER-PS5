/*
 * evo_keyboard.c — Global keyboard modal implementation.
 * Supports both Native PlayStation 5 IME Dialog and Custom Virtual Keyboard.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "evo_keyboard.h"
#include "evo_ime_dialog.h"
#include "evo_feedback.h"
#include "evo_boot_log.h"       /* #34: every native-IME step lands in evo.log */
#include "evo_boot_trace.h"     /* evo_bt: the pre-unjail probe also goes to klog */
#include "evo_rmlui_bridge.h"   /* #81: the modal is an RmlUi document now */
#include "qrcodegen.h"          /* the phone-link QR code */
#if defined(EVO_APP_MODULE) && (defined(EVO_TARGET_PS5) || defined(__FreeBSD__))
#include <unistd.h>             /* the phone link: this console's own address */
#include <sys/socket.h>
#include <netinet/in.h>
#endif

#define KB_MAX_BUF 520   /* 512 chars: the phone page and a long playlist URL */

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
static int g_native_ime_broken = 0;   /* latched after a failed native open */

/* package-app.sh --virtual-keyboard (debug builds only): the dev remote can
 * neither press nor see the system IME, so pin EVO's own keyboard. */
#if defined(EVO_FORCE_VIRTUAL_KEYBOARD)
static int g_kb_type = EVO_KEYBOARD_TYPE_VIRTUAL;

void evo_keyboard_set_type(int type)
{
    (void)type;
}
#else
static int g_kb_type = EVO_KEYBOARD_TYPE_NATIVE;

void evo_keyboard_set_type(int type)
{
    g_kb_type = (type == EVO_KEYBOARD_TYPE_VIRTUAL) ? EVO_KEYBOARD_TYPE_VIRTUAL : EVO_KEYBOARD_TYPE_NATIVE;
}
#endif

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


/*
 * Key layout, native-IME style: a number row with Backspace at its end, three
 * letter rows, and a bottom row (symbols switch, @ and :, a wide Space, / and -,
 * Done). Rows are centred on an 11-column grid; wide keys span several columns.
 * The kit draws it (ui_kit KeyboardOverlay); this file owns focus and text.
 */
#define KB_COLS 11
#define KB_CH(t, s)  { EVO_KB_KEY_CHAR, 1, t, s }
#define KB_KIND(k, n) { k, n, "", "" }

#define KB_BOTTOM_ROW \
    KB_KIND(EVO_KB_KEY_LAYOUT, 2), KB_CH("@", ""), KB_CH(":", ""), \
    KB_KIND(EVO_KB_KEY_SPACE, 3), KB_CH("/", ""), KB_CH("-", ""), KB_KIND(EVO_KB_KEY_DONE, 2)

static const evo_kb_key_t KB_L0[] = {
    KB_CH("1", ""), KB_CH("2", ""), KB_CH("3", ""), KB_CH("4", ""), KB_CH("5", ""),
    KB_CH("6", ""), KB_CH("7", ""), KB_CH("8", ""), KB_CH("9", ""), KB_CH("0", ""),
    KB_KIND(EVO_KB_KEY_BACKSPACE, 1)
};
static const evo_kb_key_t KB_L1[] = {
    KB_CH("q", "Q"), KB_CH("w", "W"), KB_CH("e", "E"), KB_CH("r", "R"), KB_CH("t", "T"),
    KB_CH("y", "Y"), KB_CH("u", "U"), KB_CH("i", "I"), KB_CH("o", "O"), KB_CH("p", "P")
};
static const evo_kb_key_t KB_L2[] = {
    KB_CH("a", "A"), KB_CH("s", "S"), KB_CH("d", "D"), KB_CH("f", "F"), KB_CH("g", "G"),
    KB_CH("h", "H"), KB_CH("j", "J"), KB_CH("k", "K"), KB_CH("l", "L"), KB_CH("'", "")
};
static const evo_kb_key_t KB_L3[] = {
    KB_KIND(EVO_KB_KEY_SHIFT, 1),
    KB_CH("z", "Z"), KB_CH("x", "X"), KB_CH("c", "C"), KB_CH("v", "V"), KB_CH("b", "B"),
    KB_CH("n", "N"), KB_CH("m", "M"), KB_CH(",", ""), KB_CH(".", ""), KB_CH("?", "")
};
static const evo_kb_key_t KB_L4[] = { KB_BOTTOM_ROW };

static const evo_kb_key_t KB_S0[] = {
    KB_CH("1", ""), KB_CH("2", ""), KB_CH("3", ""), KB_CH("4", ""), KB_CH("5", ""),
    KB_CH("6", ""), KB_CH("7", ""), KB_CH("8", ""), KB_CH("9", ""), KB_CH("0", ""),
    KB_KIND(EVO_KB_KEY_BACKSPACE, 1)
};
static const evo_kb_key_t KB_S1[] = {
    KB_CH("!", ""), KB_CH("@", ""), KB_CH("#", ""), KB_CH("$", ""), KB_CH("%", ""),
    KB_CH("^", ""), KB_CH("&", ""), KB_CH("*", ""), KB_CH("(", ""), KB_CH(")", "")
};
static const evo_kb_key_t KB_S2[] = {
    KB_CH("-", ""), KB_CH("_", ""), KB_CH("=", ""), KB_CH("+", ""), KB_CH("[", ""),
    KB_CH("]", ""), KB_CH("{", ""), KB_CH("}", ""), KB_CH("<", ""), KB_CH(">", "")
};
static const evo_kb_key_t KB_S3[] = {
    KB_CH("/", ""), KB_CH("\\", ""), KB_CH("|", ""), KB_CH(":", ""), KB_CH(";", ""), KB_CH("\"", ""),
    KB_CH("'", ""), KB_CH("~", ""), KB_CH("`", ""), KB_CH("?", ""), KB_CH(",", "")
};
static const evo_kb_key_t KB_S4[] = { KB_BOTTOM_ROW };

/* The digits-only pad (#227): three columns, backspace and Done beside the 0. */
static const evo_kb_key_t KB_P0[] = { KB_CH("1", ""), KB_CH("2", ""), KB_CH("3", "") };
static const evo_kb_key_t KB_P1[] = { KB_CH("4", ""), KB_CH("5", ""), KB_CH("6", "") };
static const evo_kb_key_t KB_P2[] = { KB_CH("7", ""), KB_CH("8", ""), KB_CH("9", "") };
static const evo_kb_key_t KB_P3[] = { KB_KIND(EVO_KB_KEY_BACKSPACE, 1), KB_CH("0", ""), KB_KIND(EVO_KB_KEY_DONE, 1) };
#define KB_PAD_COLS 3

#define KB_N(a) ((int)(sizeof(a) / sizeof((a)[0])))
static const evo_kb_key_t *const KB_PAD_ROWS[EVO_KB_ROWS] = { KB_P0, KB_P1, KB_P2, KB_P3, NULL };
static const int KB_PAD_LEN[EVO_KB_ROWS] = { KB_N(KB_P0), KB_N(KB_P1), KB_N(KB_P2), KB_N(KB_P3), 0 };
static const evo_kb_key_t *const KB_ROWS[EVO_KB_LAYERS][EVO_KB_ROWS] = {
    { KB_L0, KB_L1, KB_L2, KB_L3, KB_L4 },
    { KB_S0, KB_S1, KB_S2, KB_S3, KB_S4 },
};
static const int KB_ROW_LEN[EVO_KB_LAYERS][EVO_KB_ROWS] = {
    { KB_N(KB_L0), KB_N(KB_L1), KB_N(KB_L2), KB_N(KB_L3), KB_N(KB_L4) },
    { KB_N(KB_S0), KB_N(KB_S1), KB_N(KB_S2), KB_N(KB_S3), KB_N(KB_S4) },
};

typedef struct {
    int             is_open;
    int             is_native_active;
    int             phone;       /* the phone page can answer this prompt */
    int             secret;      /* a password: never echoed to the phone page */
    int             number;      /* digits-only port entry (#227) */
    int             noport;      /* online, but no port is free: no QR, "Set port" on R3 */
    char            prompt[128]; /* the title without the phone-link hint */
    char            phone_label[48]; /* "phone: ip:port", drawn under the QR */
    char            title[128];
    char            buffer[KB_MAX_BUF];
    int             max_len;
    int             layer;       /* 0 = letters, 1 = symbols */
    int             shift;       /* 0 = off, 1 = next letter only, 2 = caps lock */
    int             row;         /* 0..4 key rows, EVO_KB_CLEAR_ROW = the field's clear button */
    int             col;         /* key index within the row */
    float           xcol;        /* where the player is, in grid columns: up / down keep it */
    int             cursor;      /* byte offset of the caret in buffer; -1 = the end */
    uint32_t        blink_timer;
    evo_keyboard_cb callback;
    void           *userdata;
} evo_keyboard_state_t;

static evo_keyboard_state_t g_kb;
/* The text prompt that was open when R3 started a port entry on top of it. */
static evo_keyboard_state_t g_kb_saved;
static int                  g_kb_has_saved;
static int                  g_open_number;   /* kb_open: make this one the digits pad */

/* Live check of the number being typed: recomputed only when the text changes. */
static char g_chk_for[16];
static int  g_chk_kind;
static char g_chk_title[48];
static char g_chk_notice[96];

/* The phone link as a QR code: encoded once when a phone prompt opens, drawn
 * over the framebuffer by evo_screen_keyboard(). Version 5 at most (a plain
 * http://ip:port/input link is version 2-3). */
#define KB_QR_MAX_VER 5
static uint8_t g_qr[qrcodegen_BUFFER_LEN_FOR_VERSION(KB_QR_MAX_VER)];
static int     g_qr_ok;
static uint8_t g_qr_mod[(4 * KB_QR_MAX_VER + 17) * (4 * KB_QR_MAX_VER + 17)];
static int     g_qr_n;

/*
 * Remote text entry. The dev remote polls /mnt/usb0/evo_cmd on its own thread,
 * so it parks the request here and evo_keyboard_update() (frame loop) applies
 * it, the same way evo_usb_remote parks injected buttons.
 */
static volatile int g_pending_text;
static volatile int g_pending_submit;
static volatile unsigned g_pending_seq;   /* prompt the parked request was meant for; 0 = any */
static volatile unsigned g_kb_seq;        /* bumps on every open, so a late phone POST cannot hit the next prompt */
static char         g_pending_buf[KB_MAX_BUF];

static void submit_text(void);

int evo_keyboard_queue_text(const char *text)
{
    if (!g_kb.is_open || g_kb.is_native_active)
        return 0;
    g_pending_seq = 0;
    snprintf(g_pending_buf, sizeof g_pending_buf, "%s", text ? text : "");
    g_pending_text = 1;
    return 1;
}

int evo_keyboard_queue_submit(void)
{
    if (!g_kb.is_open || g_kb.is_native_active)
        return 0;
    g_pending_submit = 1;
    return 1;
}

int evo_keyboard_phone_state(evo_keyboard_phone_state_t *st)
{
    if (!st || !g_kb.is_open || !g_kb.phone || g_kb.is_native_active) return 0;
    st->seq = g_kb_seq;
    st->max_len = g_kb.max_len;
    st->secret = g_kb.secret;
    snprintf(st->prompt, sizeof st->prompt, "%s", g_kb.prompt);
    snprintf(st->text, sizeof st->text, "%s", g_kb.secret ? "" : g_kb.buffer);
    return 1;
}

int evo_keyboard_phone_submit(unsigned seq, const char *text, int submit)
{
    if (!g_kb.is_open || !g_kb.phone || g_kb.is_native_active || seq != g_kb_seq)
        return 0;
    snprintf(g_pending_buf, sizeof g_pending_buf, "%s", text ? text : "");
    g_pending_seq = seq;
    g_pending_text = 1;
    if (submit) g_pending_submit = 1;
    return 1;
}

void evo_keyboard_mark_secret(void)
{
    if (g_kb.is_open) g_kb.secret = 1;
}

/*
 * This console's address on the network, for the phone link: a UDP socket
 * "connected" to a public address sends nothing, but makes the kernel pick the
 * outgoing interface, and getsockname() then names it. Empty when offline.
 */
static void phone_local_ip(char *out, size_t cap)
{
    out[0] = '\0';
#if defined(EVO_APP_MODULE) && (defined(EVO_TARGET_PS5) || defined(__FreeBSD__))
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return;
    struct sockaddr_in to;
    memset(&to, 0, sizeof to);
    to.sin_family = AF_INET;
    to.sin_port = htons(80);
    to.sin_addr.s_addr = htonl(0x08080808u);     /* 8.8.8.8 */
    struct sockaddr_in me;
    socklen_t ml = sizeof me;
    if (connect(s, (struct sockaddr *)&to, sizeof to) == 0 &&   /* connect-ok: UDP, sends nothing */
        getsockname(s, (struct sockaddr *)&me, &ml) == 0 && me.sin_addr.s_addr != 0) {
        uint32_t a = ntohl(me.sin_addr.s_addr);
        snprintf(out, cap, "%u.%u.%u.%u", (a >> 24) & 255, (a >> 16) & 255, (a >> 8) & 255, a & 255);
    }
    close(s);
#else
    (void)cap;
#endif
}

static void kb_open(const char *title, const char *initial_value, int max_len,
                    evo_keyboard_cb on_submit, void *userdata, int phone);

void evo_keyboard_open(const char *title,
                       const char *initial_value,
                       int max_len,
                       evo_keyboard_cb on_submit,
                       void *userdata)
{
    kb_open(title, initial_value, max_len, on_submit, userdata, 0);
}

void evo_keyboard_open_phone(const char *title,
                             const char *initial_value,
                             int max_len,
                             evo_keyboard_cb on_submit,
                             void *userdata)
{
    kb_open(title, initial_value, max_len, on_submit, userdata, 1);
}

int evo_keyboard_phone_info(char *title, size_t tcap)
{
    if (!g_kb.is_open || !g_kb.phone || g_kb.is_native_active) return 0;
    if (title && tcap) snprintf(title, tcap, "%s", g_kb.prompt);
    return 1;
}

/* The phone link of the open prompt: the QR and the "phone: ip:port" title, from the
 * port the log server really got. Online but no port: noport, and a notice instead. */
static void kb_setup_phone(void)
{
    char ip[32];
    g_qr_ok = 0;
    g_kb.phone = 0;
    g_kb.noport = 0;
    g_kb.phone_label[0] = '\0';
    strncpy(g_kb.title, g_kb.prompt, sizeof(g_kb.title) - 1);
    phone_local_ip(ip, sizeof ip);
    if (!ip[0]) return;
    const int port = evo_log_server_port();
    if (port <= 0) { g_kb.noport = 1; return; }
    g_kb.phone = 1;
    snprintf(g_kb.phone_label, sizeof g_kb.phone_label, "phone: %s:%d", ip, port);
    char url[64];
    uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(KB_QR_MAX_VER)];
    snprintf(url, sizeof url, "http://%s:%d/input", ip, port);
    g_qr_ok = qrcodegen_encodeText(url, tmp, g_qr, qrcodegen_Ecc_LOW,
                                   qrcodegen_VERSION_MIN, KB_QR_MAX_VER,
                                   qrcodegen_Mask_AUTO, true);
    if (g_qr_ok) {
        g_qr_n = qrcodegen_getSize(g_qr);
        for (int y = 0; y < g_qr_n; y++)
            for (int x = 0; x < g_qr_n; x++)
                g_qr_mod[y * g_qr_n + x] = qrcodegen_getModule(g_qr, x, y) ? 1 : 0;
    }
}

static void kb_phone_info_refresh(void)
{
    kb_setup_phone();
}

int evo_keyboard_wants_r3(void)
{
    return g_kb.is_open && g_kb.noport && !g_kb.number && !g_kb.is_native_active;
}

void evo_keyboard_local_ip(char *out, size_t cap)
{
    phone_local_ip(out, cap);
}

void evo_keyboard_open_number(const char *title, const char *initial_value, int max_len,
                              evo_keyboard_cb on_submit, void *userdata)
{
    g_open_number = 1;
    kb_open(title, initial_value, max_len, on_submit, userdata, 0);
}

static void kb_open(const char *title, const char *initial_value, int max_len,
                    evo_keyboard_cb on_submit, void *userdata, int phone)
{
    memset(&g_kb, 0, sizeof(g_kb));
    g_kb_seq++;
    g_qr_ok = 0;
    g_chk_for[0] = '\x01';   /* force the first check */
    g_kb.is_open = 1;
    if (title) strncpy(g_kb.title, title, sizeof(g_kb.title) - 1);
    else strncpy(g_kb.title, "ENTER TEXT", sizeof(g_kb.title) - 1);
    strncpy(g_kb.prompt, g_kb.title, sizeof(g_kb.prompt) - 1);

    /* The system IME is a separate dialog: it can neither show the phone link
     * nor take text from the phone, so a phone prompt always uses EVO's own.
     * The port entry is digits only: no phone link, no IME. */
    (void)phone;   /* every prompt gets the phone link when the console has an address */
    g_kb.number = g_open_number;
    g_open_number = 0;
    if (!g_kb.number) kb_setup_phone();

    if (initial_value) {
        g_kb.cursor = -1;
        strncpy(g_kb.buffer, initial_value, sizeof(g_kb.buffer) - 1);
    }

    g_kb.max_len = (max_len > 0 && max_len < KB_MAX_BUF) ? max_len : (KB_MAX_BUF - 1);
    g_kb.layer = 0;
    g_kb.shift = 0;
    g_kb.row = 1;    /* Start on 'q' / first letter row */
    g_kb.col = 0;
    g_kb.xcol = 0.5f;
    if (g_kb.number) { g_kb.col = 1; g_kb.xcol = 1.5f; }   /* the 5 */
    g_kb.blink_timer = 0;
    g_kb.callback = on_submit;
    g_kb.userdata = userdata;

#if defined(EVO_TARGET_PS5) || defined(__FreeBSD__)
    if (g_kb_type == EVO_KEYBOARD_TYPE_NATIVE && !g_native_ime_broken && !g_kb.phone &&
        !g_kb.number && !g_kb.noport) {
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
    /* Apply anything the dev remote parked, on the frame loop's thread. */
    if (g_pending_seq && g_pending_seq != g_kb_seq) {   /* meant for a prompt that is gone */
        g_pending_text = 0;
        g_pending_submit = 0;
    }
    if (g_pending_text) {
        g_pending_text = 0;
        if (g_kb.is_open && !g_kb.is_native_active) {
            snprintf(g_kb.buffer, sizeof g_kb.buffer, "%s", g_pending_buf);
            g_kb.cursor = -1;
            if ((int)strlen(g_kb.buffer) > g_kb.max_len)
                g_kb.buffer[g_kb.max_len] = '\0';
        }
    }
    if (g_pending_submit) {
        g_pending_submit = 0;
        if (g_kb.is_open && !g_kb.is_native_active)
            submit_text();     /* fires the callback and closes the modal */
    }

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


/* The caret as a byte offset into the buffer, clamped. -1 means the end, which is
 * where every external write (open, phone text) leaves it. */
static size_t kb_cursor(void)
{
    size_t len = strlen(g_kb.buffer);
    if (g_kb.cursor < 0 || (size_t)g_kb.cursor > len) g_kb.cursor = (int)len;
    return (size_t)g_kb.cursor;
}

/* Move the caret one character (UTF-8 aware): dir -1 = left, +1 = right. */
static void move_cursor(int dir)
{
    size_t len = strlen(g_kb.buffer);
    size_t cur = kb_cursor();
    if (dir < 0 && cur > 0) {
        do { cur--; } while (cur > 0 && ((unsigned char)g_kb.buffer[cur] & 0xC0) == 0x80);
    } else if (dir > 0 && cur < len) {
        do { cur++; } while (cur < len && ((unsigned char)g_kb.buffer[cur] & 0xC0) == 0x80);
    } else {
        evo_feedback(EVO_FB_BOUNDARY);
        return;
    }
    g_kb.cursor = (int)cur;
    evo_feedback(EVO_FB_MOVE);
}

static void backspace_char(void);
static void submit_text(void);

static void insert_str(const char *str)
{
    size_t n = strlen(str);
    if (g_kb.number)   /* the port pad takes digits and nothing else */
        for (size_t i = 0; i < n; i++)
            if (str[i] < '0' || str[i] > '9') { evo_feedback(EVO_FB_BOUNDARY); return; }
    size_t len = strlen(g_kb.buffer);
    if (n > 0 && (int)(len + n) <= g_kb.max_len && len + n < sizeof(g_kb.buffer)) {
        size_t cur = kb_cursor();
        memmove(g_kb.buffer + cur + n, g_kb.buffer + cur, len - cur + 1);
        memcpy(g_kb.buffer + cur, str, n);
        g_kb.cursor = (int)(cur + n);
        evo_feedback(EVO_FB_MOVE);
    } else {
        evo_feedback(EVO_FB_BOUNDARY);
    }
}

static void insert_char(char c)
{
    char one[2] = { c, 0 };
    insert_str(one);
}

/* ---- grid geometry: rows are centred on KB_COLS columns, wide keys span several ---- */

static int kb_row_len(int row)
{
    if (row < 0 || row >= EVO_KB_ROWS) return 0;
    return g_kb.number ? KB_PAD_LEN[row] : KB_ROW_LEN[g_kb.layer][row];
}

static int kb_cols(void)     { return g_kb.number ? KB_PAD_COLS : KB_COLS; }
static int kb_last_row(void) { return g_kb.number ? 3 : EVO_KB_ROWS - 1; }

static const evo_kb_key_t *kb_key(int row, int col)
{
    if (row < 0 || row >= EVO_KB_ROWS || col < 0 || col >= kb_row_len(row)) return NULL;
    return g_kb.number ? &KB_PAD_ROWS[row][col] : &KB_ROWS[g_kb.layer][row][col];
}

static float kb_row_start(int row)
{
    int cols = 0;
    for (int c = 0; c < kb_row_len(row); c++) cols += kb_key(row, c)->span;
    return (kb_cols() - cols) * 0.5f;
}

static float kb_key_start(int row, int col)
{
    float x = kb_row_start(row);
    for (int c = 0; c < col; c++) x += kb_key(row, c)->span;
    return x;
}

static float kb_key_centre(int row, int col)
{
    return kb_key_start(row, col) + kb_key(row, col)->span * 0.5f;
}

/* The key of `row` under grid column `x` (the nearest one when x is in a margin). */
static int kb_key_under(int row, float x)
{
    int best = 0;
    float best_d = 1e9f;
    for (int c = 0; c < kb_row_len(row); c++) {
        float d = x - kb_key_centre(row, c);
        if (d < 0) d = -d;
        if (d < best_d) { best_d = d; best = c; }
    }
    return best;
}

static void kb_move_to_row(int row)
{
    g_kb.row = row;
    g_kb.col = kb_key_under(row, g_kb.xcol);
}

static void kb_shift_cycle(void)
{
    if (g_kb.number) return;
    if (g_kb.layer != 0) { evo_feedback(EVO_FB_BOUNDARY); return; }
    g_kb.shift = (g_kb.shift + 1) % 3;   /* off -> next letter -> caps lock -> off */
    evo_feedback(EVO_FB_TOGGLE);
}

/* What the key types now: its shifted form under shift, or the capital of a letter. */
static void kb_press(const evo_kb_key_t *k)
{
    switch (k->kind) {
    case EVO_KB_KEY_CHAR:
        if (g_kb.shift && k->shifted && k->shifted[0]) insert_str(k->shifted);
        else insert_str(k->text);
        if (g_kb.shift == 1) g_kb.shift = 0;
        break;
    case EVO_KB_KEY_SPACE:
        insert_char(' ');
        break;
    case EVO_KB_KEY_SHIFT:
        kb_shift_cycle();
        break;
    case EVO_KB_KEY_BACKSPACE:
        backspace_char();
        break;
    case EVO_KB_KEY_LAYOUT:
        g_kb.layer ^= 1;
        g_kb.shift = 0;
        if (g_kb.col >= kb_row_len(g_kb.row)) g_kb.col = kb_row_len(g_kb.row) - 1;
        evo_feedback(EVO_FB_TOGGLE);
        break;
    case EVO_KB_KEY_DONE:
        submit_text();
        break;
    }
}

static void backspace_char(void)
{
    size_t len = strlen(g_kb.buffer);
    size_t cur = kb_cursor();
    if (cur > 0) {   /* delete the character before the caret */
        size_t start = cur - 1;
        while (start > 0 && ((unsigned char)g_kb.buffer[start] & 0xC0) == 0x80) start--;
        memmove(g_kb.buffer + start, g_kb.buffer + cur, len - cur + 1);
        g_kb.cursor = (int)start;
        evo_feedback(EVO_FB_TOGGLE);
    } else {
        evo_feedback(EVO_FB_BOUNDARY);
    }
}

static void kb_phone_info_refresh(void);

/* Leave the open prompt. A port entry that R3 started on top of a text prompt
 * hands that prompt back, with the phone link worked out again from the new port. */
static void kb_leave(void)
{
    const int was_number = g_kb.number;
    g_kb.is_open = 0;
    if (was_number && g_kb_has_saved) {
        g_kb = g_kb_saved;
        g_kb_has_saved = 0;
        g_kb_seq++;
        kb_phone_info_refresh();
    }
}

static void submit_text(void)
{
    if (g_kb.number && g_chk_kind != 1) {   /* only a port that tested free is saved */
        evo_feedback(EVO_FB_BOUNDARY);
        return;
    }
    if (g_kb.callback) {
        g_kb.callback(g_kb.buffer, g_kb.userdata);
    }
    evo_feedback(EVO_FB_CONFIRM);
    kb_leave();
}

extern void prospero_settings_save(void);
void toast(const char *title, const char *msg);

/* R3 on "PHONE LINK OFF": set the port without leaving the prompt. */
static void port_entry_done(const char *text, void *ud)
{
    (void)ud;
    int port = atoi(text);
    if (evo_log_server_try_port(port) == 0) {
        prospero_settings_save();
        toast("PHONE & LOG PORT", "Saved");
    }
}

static void kb_open_port_entry(void)
{
    char init[8];
    snprintf(init, sizeof init, "%d", evo_log_server_saved_port());
    g_kb_saved = g_kb;
    g_kb_has_saved = 1;
    evo_keyboard_open_number("PHONE & LOG PORT (1024-65535)", init, 5, port_entry_done, NULL);
}

#define EVO_PAD_R3        0x0004
#define EVO_PAD_OPTIONS   0x0008
#define EVO_PAD_UP        0x0010
#define EVO_PAD_RIGHT     0x0020
#define EVO_PAD_DOWN      0x0040
#define EVO_PAD_LEFT      0x0080
#define EVO_PAD_L2        0x0100
#define EVO_PAD_R2        0x0200
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

    /* D-PAD Navigation. Up from the top row goes to the field's clear button (when
     * there is text to clear), then wraps to the bottom row. */
    if (pressed & EVO_PAD_UP) {
        if (g_kb.row == EVO_KB_CLEAR_ROW)  kb_move_to_row(kb_last_row());
        else if (g_kb.row == 0 && g_kb.buffer[0]) { g_kb.row = EVO_KB_CLEAR_ROW; g_kb.col = 0; }
        else if (g_kb.row == 0)            kb_move_to_row(kb_last_row());
        else                               kb_move_to_row(g_kb.row - 1);
        evo_feedback(EVO_FB_MOVE);
        return 1;
    }

    if (pressed & EVO_PAD_DOWN) {
        if (g_kb.row == EVO_KB_CLEAR_ROW)       kb_move_to_row(0);
        else if (g_kb.row == kb_last_row())     kb_move_to_row(0);
        else                                    kb_move_to_row(g_kb.row + 1);
        evo_feedback(EVO_FB_MOVE);
        return 1;
    }

    if (pressed & (EVO_PAD_LEFT | EVO_PAD_RIGHT)) {
        if (g_kb.row == EVO_KB_CLEAR_ROW) { evo_feedback(EVO_FB_BOUNDARY); return 1; }
        int n = kb_row_len(g_kb.row);
        if (n <= 0) { evo_feedback(EVO_FB_BOUNDARY); return 1; }   /* no keys: no modulo by zero */
        g_kb.col = (g_kb.col + ((pressed & EVO_PAD_RIGHT) ? 1 : n - 1)) % n;
        g_kb.xcol = kb_key_centre(g_kb.row, g_kb.col);
        evo_feedback(EVO_FB_MOVE);
        return 1;
    }

    /* Shortcuts, the same buttons as the native keyboard */
    if (pressed & EVO_PAD_SQUARE) { backspace_char(); return 1; }       /* SQUARE: Backspace */
    if (pressed & EVO_PAD_TRIANGLE) { insert_char(' '); return 1; }     /* TRIANGLE: Space */
    if (pressed & (EVO_PAD_R2 | EVO_PAD_OPTIONS)) { submit_text(); return 1; }  /* R2 / OPTIONS: Done */
    if (pressed & EVO_PAD_L2) { kb_shift_cycle(); return 1; }           /* L2: Shift, again = Caps Lock */

    if (pressed & EVO_PAD_CIRCLE) { /* CIRCLE: Cancel */
        evo_feedback(EVO_FB_CANCEL);
        kb_leave();
        return 1;
    }

    if ((pressed & EVO_PAD_R3) && g_kb.noport && !g_kb.number) {
        kb_open_port_entry();
        return 1;
    }

    /* L1 / R1 move the caret, to fix a typo in the middle of the text. */
    if (pressed & EVO_PAD_L1) { move_cursor(-1); return 1; }
    if (pressed & EVO_PAD_R1) { move_cursor(+1); return 1; }

    /* CROSS: press the focused key, or clear the field */
    if (pressed & EVO_PAD_CROSS) {
        if (g_kb.row == EVO_KB_CLEAR_ROW) {
            g_kb.buffer[0] = '\0';
            g_kb.cursor = -1;
            kb_move_to_row(0);
            evo_feedback(EVO_FB_TOGGLE);
        } else {
            const evo_kb_key_t *k = kb_key(g_kb.row, g_kb.col);
            if (k) kb_press(k);
            if (kb_key(g_kb.row, g_kb.col)) g_kb.xcol = kb_key_centre(g_kb.row, g_kb.col);
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
/* White card with a 4-module quiet zone, in the right margin beside the panel
 * (the panel spans x 260..1660). 0xAABBGGRR: opaque white / opaque black. */
/* What the pad tells the player about the number so far (green / red / amber). */
static void kb_port_check(void)
{
    if (strcmp(g_chk_for, g_kb.buffer) == 0) return;
    snprintf(g_chk_for, sizeof g_chk_for, "%s", g_kb.buffer);
    const int port = g_kb.buffer[0] ? atoi(g_kb.buffer) : 0;
    if (!g_kb.buffer[0]) {
        g_chk_kind = 2;
        snprintf(g_chk_title, sizeof g_chk_title, "TYPE A PORT");
        snprintf(g_chk_notice, sizeof g_chk_notice, "Use a number from 1024 to 65535.");
    } else if (port < 1024 || port > 65535) {
        g_chk_kind = 0;
        snprintf(g_chk_title, sizeof g_chk_title, "NOT A VALID PORT");
        snprintf(g_chk_notice, sizeof g_chk_notice, "Use a number from 1024 to 65535.");
    } else if (evo_log_server_probe_port(port) != 0) {
        g_chk_kind = 0;
        snprintf(g_chk_title, sizeof g_chk_title, "PORT %d IS TAKEN", port);
        snprintf(g_chk_notice, sizeof g_chk_notice, "Something else is using it. Type another port.");
    } else {
        g_chk_kind = 1;
        snprintf(g_chk_title, sizeof g_chk_title, "PORT %d IS FREE", port);
        snprintf(g_chk_notice, sizeof g_chk_notice, "Save it and the phone link and QR code come back at once.");
    }
}

static void draw_phone_qr(uint32_t *fb)
{
    const int n = qrcodegen_getSize(g_qr);
    const int px = 6, quiet = 4;
    const int side = (n + 2 * quiet) * px;
    const int x0 = 1680, y0 = 140;
    for (int y = 0; y < side; y++)
        for (int x = 0; x < side; x++) {
            const int mx = x / px - quiet, my = y / px - quiet;
            const int dark = mx >= 0 && my >= 0 && mx < n && my < n &&
                             qrcodegen_getModule(g_qr, mx, my);
            fb[(y0 + y) * 1920 + x0 + x] = dark ? 0xFF000000u : 0xFFFFFFFFu;
        }
}

void evo_screen_keyboard(uint32_t *fb)
{
    evo_keyboard_params_t p;
    memset(&p, 0, sizeof(p));

    if (!g_kb.is_open) {
        evo_rmlui_update_keyboard(&p);   /* visible = 0 -> hide the doc */
        return;
    }

    /* A port that comes up while the card is showing (just set, or one that freed
     * up in the background) brings the QR back without reopening the prompt. */
    if (g_kb.noport && !g_kb.number && evo_log_server_port() > 0) kb_setup_phone();

    p.visible     = 1;
    p.native_only = g_kb.is_native_active;
    p.title       = g_kb.title;
    p.text        = g_kb.buffer;
    p.mode_label  = g_kb.number ? "NUMBERS"
                  : g_kb.layer ? "SYMBOLS"
                  : g_kb.shift == 1 ? "SHIFT"
                  : g_kb.shift == 2 ? "CAPS LOCK" : "LOWERCASE";
    for (int l = 0; l < EVO_KB_LAYERS; l++)
        for (int r = 0; r < EVO_KB_ROWS; r++) {
            p.key_rows[l][r]    = g_kb.number ? KB_PAD_ROWS[r] : KB_ROWS[l][r];
            p.key_row_len[l][r] = g_kb.number ? KB_PAD_LEN[r] : KB_ROW_LEN[l][r];
        }
    p.key_columns   = kb_cols();
    p.layer         = g_kb.layer;
    p.shift_state   = g_kb.shift;
    p.clear_visible = g_kb.buffer[0] != '\0';
    p.len       = (int)strlen(g_kb.buffer);
    p.cursor    = (int)kb_cursor();
    p.max_len   = g_kb.max_len;
    p.focus_row = g_kb.row;
    p.focus_col = g_kb.col;
    if (g_kb.phone && g_qr_ok) {
        p.qr_size    = g_qr_n;
        p.qr_modules = g_qr_mod;
        p.phone_title = g_kb.phone_label;
    }
    if (g_kb.number) {
        kb_port_check();
        p.phone_title  = g_chk_title;
        p.phone_notice = g_chk_notice;
        p.phone_kind   = g_chk_kind;
    } else if (g_kb.noport) {
        static char notice[128];
        snprintf(notice, sizeof notice,
                 "Ports %d to %d are all taken, so typing from a phone is off. Set a new port to turn it back on.",
                 evo_log_server_saved_port(), evo_log_server_saved_port() + 9);
        p.phone_title  = "PHONE LINK OFF";
        p.phone_notice = notice;
        p.phone_kind   = 0;
        p.phone_action = "Set port";
    }

    g_kb.blink_timer = (g_kb.blink_timer + 1) % 60;
    p.show_caret = (g_kb.blink_timer < 36);

    evo_rmlui_update_keyboard(&p);
    evo_rmlui_render_keyboard(fb, 1920, 1080);
    if (g_kb.phone && g_qr_ok) draw_phone_qr(fb);
}

