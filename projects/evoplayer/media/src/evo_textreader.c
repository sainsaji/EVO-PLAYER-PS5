/*
 * evo_textreader — see evo_textreader.h for what this is and why.
 *
 * Deliberately free of any dependency on the player: no FFmpeg, no VideoOut,
 * no globals from main.c. It takes a path and a measuring function and gives
 * back an array of lines. That is what lets tools/uiview.sh render the reader
 * on the host, which is the only way to check text layout without a console.
 */
#include "evo_textreader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * UTF-8 folding
 *
 * The font has ASCII and nothing else. Rather than drop what it cannot draw -
 * which silently deletes the apostrophes and dashes that word processors
 * insert on your behalf, changing the text - each codepoint is folded to its
 * closest ASCII spelling. A curly quote becomes a straight one, an em dash
 * becomes a hyphen, an ellipsis becomes three periods.
 *
 * Anything with no sensible ASCII spelling becomes '.', and an invalid byte
 * becomes '.' too rather than aborting the load: a log with one corrupt line
 * is still worth reading.
 * ------------------------------------------------------------------------ */

/* Decode one UTF-8 sequence. Returns the codepoint and advances *p.
 * On a malformed sequence, consumes exactly one byte and returns -1, so the
 * caller always makes progress. */
static long utf8_next(const unsigned char **p, const unsigned char *end)
{
    const unsigned char *s = *p;
    unsigned char c = *s;
    int need;
    long cp;

    if (c < 0x80) { *p = s + 1; return c; }

    if ((c & 0xE0) == 0xC0) { need = 1; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { need = 2; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { need = 3; cp = c & 0x07; }
    else { *p = s + 1; return -1; }          /* stray continuation or 5-byte */

    /* Truncation needs no separate check: the loop below refuses any
     * continuation byte at or past `end`, which covers a sequence cut off by
     * the read cap as well as one cut off by a corrupt file. */
    for (int i = 1; i <= need; i++) {
        if (s + i >= end || (s[i] & 0xC0) != 0x80) { *p = s + 1; return -1; }
        cp = (cp << 6) | (s[i] & 0x3F);
    }

    *p = s + need + 1;
    return cp;
}

/*
 * Fold a codepoint to ASCII. Writes up to 3 bytes, returns how many.
 *
 * The mappings are the ones that actually show up in text people read: the
 * quotes and dashes Word and web pages produce, the ellipsis, the non-breaking
 * space, and the bullet at the front of a list item.
 */
static int fold_cp(long cp, char out[4])
{
    switch (cp) {
    case 0x00A0: out[0] = ' ';  return 1;    /* no-break space */
    case 0x2018:                              /* ' */
    case 0x2019: out[0] = '\''; return 1;    /* ' - the apostrophe Word inserts */
    case 0x201A: out[0] = ','; return 1;
    case 0x201C:                              /* " */
    case 0x201D: out[0] = '"';  return 1;    /* " */
    case 0x2013:                              /* en dash */
    case 0x2014: out[0] = '-';  return 1;    /* em dash */
    case 0x2015: out[0] = '-';  return 1;
    case 0x2212: out[0] = '-';  return 1;    /* minus sign */
    case 0x2022:                              /* bullet */
    case 0x00B7: out[0] = '*';  return 1;    /* middle dot */
    case 0x2026: out[0] = '.'; out[1] = '.'; out[2] = '.'; return 3;
    case 0x00AB: out[0] = '<'; out[1] = '<'; return 2;
    case 0x00BB: out[0] = '>'; out[1] = '>'; return 2;
    case 0x2192: out[0] = '-'; out[1] = '>'; return 2;
    case 0x2190: out[0] = '<'; out[1] = '-'; return 2;
    case 0x00A9: out[0] = '('; out[1] = 'C'; out[2] = ')'; return 3;
    case 0x00AE: out[0] = '('; out[1] = 'R'; out[2] = ')'; return 3;
    case 0x2122: out[0] = 'T'; out[1] = 'M'; return 2;
    case 0x00B0: out[0] = 'd'; out[1] = 'e'; out[2] = 'g'; return 3;
    case 0x00BD: out[0] = '1'; out[1] = '/'; out[2] = '2'; return 3;
    case 0xFEFF: return 0;                    /* BOM: contributes nothing */
    default: break;
    }

    if (cp >= 0x20 && cp < 0x7F) { out[0] = (char)cp; return 1; }

    /* Latin-1 letters keep their base letter, so accented European text reads
     * as slightly-wrong English rather than as a row of dots. */
    if (cp >= 0x00C0 && cp <= 0x00FF) {
        static const char base[] =
            "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYPs"
            "aaaaaaaceeeeiiiidnooooo/ouuuuypy";
        out[0] = base[cp - 0x00C0];
        return 1;
    }

    out[0] = '.';
    return 1;
}

/* ---------------------------------------------------------------------------
 * Loading
 * ------------------------------------------------------------------------ */

static void set_title_from_path(evo_text_doc *doc, const char *path)
{
    const char *slash = strrchr(path, '/');
    const char *name = slash ? slash + 1 : path;
    size_t n = strlen(name);

    if (n >= sizeof(doc->title))
        n = sizeof(doc->title) - 1;
    memcpy(doc->title, name, n);
    doc->title[n] = 0;
}

/*
 * A cheap binary sniff over the first block. NUL bytes are the giveaway: no
 * text encoding this reader handles produces them, and opening an ELF in a
 * text reader produces a screenful of dots and a confused user.
 */
static int looks_binary(const unsigned char *p, size_t n)
{
    size_t check = n < 4096 ? n : 4096;
    size_t nul = 0;

    for (size_t i = 0; i < check; i++)
        if (p[i] == 0)
            nul++;

    return check > 0 && nul > 0;
}

evo_text_status evo_text_load(evo_text_doc *doc, const char *path)
{
    FILE *f;
    unsigned char *raw = NULL;
    size_t raw_len = 0;
    long size;

    if (!doc)
        return EVO_TEXT_ERR_OPEN;

    memset(doc, 0, sizeof(*doc));
    doc->face = 1;                    /* SUB: the comfortable reading size */
    if (path) {
        size_t n = strlen(path);
        if (n >= sizeof(doc->path))
            n = sizeof(doc->path) - 1;
        memcpy(doc->path, path, n);
        doc->path[n] = 0;
        set_title_from_path(doc, path);
    }

    f = path ? fopen(path, "rb") : NULL;
    if (!f)
        return (doc->status = EVO_TEXT_ERR_OPEN);

    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) {
        fclose(f);
        return (doc->status = EVO_TEXT_ERR_EMPTY);
    }

    doc->file_bytes = (size_t)size;
    raw_len = (size_t)size;
    if (raw_len > EVO_TEXT_MAX_BYTES) {
        raw_len = EVO_TEXT_MAX_BYTES;
        doc->truncated = 1;
    }

    raw = (unsigned char *)malloc(raw_len + 1);
    if (!raw) {
        fclose(f);
        return (doc->status = EVO_TEXT_ERR_MEMORY);
    }

    raw_len = fread(raw, 1, raw_len, f);
    fclose(f);
    raw[raw_len] = 0;

    if (raw_len == 0) {
        free(raw);
        return (doc->status = EVO_TEXT_ERR_EMPTY);
    }

    if (looks_binary(raw, raw_len)) {
        free(raw);
        return (doc->status = EVO_TEXT_ERR_BINARY);
    }

    /*
     * Fold into the document buffer. Worst case a codepoint expands to three
     * bytes, so the output can be larger than the input - size for that rather
     * than reallocating in the middle of a decode.
     */
    doc->buf = (char *)malloc(raw_len * 3 + 2);
    if (!doc->buf) {
        free(raw);
        return (doc->status = EVO_TEXT_ERR_MEMORY);
    }

    {
        const unsigned char *p = raw;
        const unsigned char *end = raw + raw_len;
        size_t out = 0;
        int last_was_cr = 0;

        while (p < end) {
            char folded[4];
            long cp = utf8_next(&p, end);
            int n;

            /* Line endings are normalised to '\n' here so the wrapper only has
             * one case to think about, and a CRLF file does not show a stray
             * glyph at the end of every line. */
            if (cp == '\r') { doc->buf[out++] = '\n'; last_was_cr = 1; continue; }
            if (cp == '\n') {
                if (last_was_cr) { last_was_cr = 0; continue; }
                doc->buf[out++] = '\n';
                continue;
            }
            last_was_cr = 0;

            if (cp == '\t') {
                /* Tabs are folded to spaces; a tab stop means nothing once the
                 * text is proportionally spaced and wrapped. */
                for (int i = 0; i < 4; i++)
                    doc->buf[out++] = ' ';
                continue;
            }

            if (cp < 0) { doc->buf[out++] = '.'; continue; }

            n = fold_cp(cp, folded);
            for (int i = 0; i < n; i++)
                doc->buf[out++] = folded[i];
        }

        doc->buf[out] = 0;
        doc->buf_len = out;
    }

    free(raw);

    if (doc->buf_len == 0) {
        free(doc->buf);
        doc->buf = NULL;
        return (doc->status = EVO_TEXT_ERR_EMPTY);
    }

    for (size_t i = 0; i < doc->buf_len; i++)
        if (doc->buf[i] == '\n')
            doc->source_lines++;
    doc->source_lines++;

    doc->status = EVO_TEXT_OK;
    return EVO_TEXT_OK;
}

void evo_text_free(evo_text_doc *doc)
{
    if (!doc)
        return;
    free(doc->buf);
    free(doc->lines);
    doc->buf = NULL;
    doc->lines = NULL;
    doc->line_count = doc->line_cap = 0;
    doc->buf_len = 0;
}

int evo_text_is_empty(const evo_text_doc *doc)
{
    return !doc || doc->status != EVO_TEXT_OK || doc->line_count == 0;
}

/* ---------------------------------------------------------------------------
 * Wrapping
 * ------------------------------------------------------------------------ */

static int push_line(evo_text_doc *doc, const char *begin, int len, int src)
{
    if (doc->line_count >= EVO_TEXT_MAX_LINES)
        return 0;

    if (doc->line_count == doc->line_cap) {
        int cap = doc->line_cap ? doc->line_cap * 2 : 1024;
        evo_text_line *n;

        if (cap > EVO_TEXT_MAX_LINES)
            cap = EVO_TEXT_MAX_LINES;
        n = (evo_text_line *)realloc(doc->lines, (size_t)cap * sizeof(*n));
        if (!n)
            return 0;
        doc->lines = n;
        doc->line_cap = cap;
    }

    doc->lines[doc->line_count].begin = begin;
    doc->lines[doc->line_count].len = len;
    doc->lines[doc->line_count].source_line = src;
    doc->line_count++;
    return 1;
}

/*
 * Wrap one source line.
 *
 * Breaks at the last space that fits. Where a single word is itself wider than
 * the column - a URL, a base64 blob, a log line with no spaces in it - it
 * breaks mid-word instead, because the alternative is a line that overruns the
 * screen. The `if (take == 0) take = 1` is what guarantees progress and makes
 * an infinite loop impossible on a pathological input.
 */
static void wrap_one(evo_text_doc *doc, const char *s, int len, int src,
                     int wrap_px, int face,
                     int (*measure)(const char *, int, int))
{
    int start = 0;

    if (len == 0) {
        push_line(doc, s, 0, src);       /* a blank line is a blank line */
        return;
    }

    while (start < len) {
        int take = 0;
        int last_space = -1;
        int w = 0;

        while (start + take < len) {
            int cw = measure(s + start + take, 1, face);

            if (w + cw > wrap_px && take > 0)
                break;

            if (s[start + take] == ' ')
                last_space = take;

            w += cw;
            take++;
        }

        if (start + take < len && last_space > 0)
            take = last_space;            /* break at the space, drop it below */

        if (take == 0)
            take = 1;                     /* never stall, even if one glyph
                                             is wider than the whole column */

        if (!push_line(doc, s + start, take, src))
            return;

        start += take;
        while (start < len && s[start] == ' ')
            start++;                      /* leading spaces of the next row */
    }
}

void evo_text_wrap(evo_text_doc *doc, int wrap_px,
                   int (*measure)(const char *, int, int))
{
    const char *p, *end;
    int src = 1;
    int keep_source_line = 0;
    int keep_offset = 0;

    if (!doc || !doc->buf || !measure || wrap_px < 16)
        return;

    /* The face matters as much as the width: every glyph advance changes with
     * it, so a cache keyed on width alone would keep a SMALL-face wrap after
     * the reader had been switched to TITLE. */
    if (doc->line_count && doc->wrap_px == wrap_px && doc->wrap_face == doc->face)
        return;

    /*
     * Remember where we are as a position in the *document*, not as a display
     * line number. Display lines are a function of the wrap width, so keeping
     * the number would move the reader somewhere else entirely every time the
     * font size changed - which is exactly when a reader must not lose its
     * place.
     */
    if (doc->line_count && doc->top < doc->line_count) {
        keep_source_line = doc->lines[doc->top].source_line;
        keep_offset = (int)(doc->lines[doc->top].begin - doc->buf);
    }

    doc->line_count = 0;

    p = doc->buf;
    end = doc->buf + doc->buf_len;

    while (p <= end) {
        const char *nl = (const char *)memchr(p, '\n', (size_t)(end - p));
        int len = (int)((nl ? nl : end) - p);

        wrap_one(doc, p, len, src, wrap_px, doc->face, measure);
        src++;

        if (!nl)
            break;
        p = nl + 1;
    }

    doc->wrap_px = wrap_px;
    doc->wrap_face = doc->face;

    /* Restore the view: the first display line at or after where we were. */
    doc->top = 0;
    if (keep_source_line) {
        for (int i = 0; i < doc->line_count; i++) {
            if (doc->lines[i].source_line > keep_source_line ||
                (int)(doc->lines[i].begin - doc->buf) >= keep_offset) {
                doc->top = i;
                break;
            }
        }
    }
}

/* ---------------------------------------------------------------------------
 * Scrolling
 * ------------------------------------------------------------------------ */

static int max_top(const evo_text_doc *doc, int visible)
{
    int m = doc->line_count - visible;
    return m > 0 ? m : 0;
}

void evo_text_scroll(evo_text_doc *doc, int delta, int visible)
{
    int m;

    if (!doc || doc->line_count == 0)
        return;

    m = max_top(doc, visible);
    doc->top += delta;

    if (doc->top > m)
        doc->top = m;
    if (doc->top < 0)
        doc->top = 0;
}

void evo_text_seek_fraction(evo_text_doc *doc, double f, int visible)
{
    int m;

    if (!doc || doc->line_count == 0)
        return;
    if (f < 0.0) f = 0.0;
    if (f > 1.0) f = 1.0;

    m = max_top(doc, visible);
    doc->top = (int)(f * m + 0.5);
    if (doc->top > m) doc->top = m;
    if (doc->top < 0) doc->top = 0;
}

double evo_text_progress(const evo_text_doc *doc, int visible)
{
    int m;

    if (!doc || doc->line_count == 0)
        return 0.0;

    m = max_top(doc, visible);
    if (m <= 0)
        return 1.0;                        /* it all fits: you are at the end */
    return (double)doc->top / (double)m;
}
