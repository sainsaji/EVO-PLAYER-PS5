/*
 * evo_langpref - preferred audio / subtitle language. See evo_langpref.h.
 */
#include "evo_langpref.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static char g_audio[4] = "";
static char g_subtitle[4] = "";

/* 639-1, 639-2/T, 639-2/B (or another common code), English name, native name.
 * Names are lower case; anything here is matched after lower-casing. */
struct lp_lang {
    const char *a2, *a3, *alt, *en, *native;
};

static const struct lp_lang k_langs[] = {
    {"it", "ita", NULL,  "italian",    "italiano"},
    {"en", "eng", NULL,  "english",    "english"},
    {"es", "spa", "esl", "spanish",    "español"},
    {"fr", "fra", "fre", "french",     "français"},
    {"de", "deu", "ger", "german",     "deutsch"},
    {"pt", "por", "pob", "portuguese", "português"},
    {"ja", "jpn", NULL,  "japanese",   "日本語"},
    {"nl", "nld", "dut", "dutch",      "nederlands"},
    {"pl", "pol", NULL,  "polish",     "polski"},
    {"ru", "rus", NULL,  "russian",    "русский"},
    {"zh", "zho", "chi", "chinese",    "中文"},
    {"ko", "kor", NULL,  "korean",     "한국어"},
    {"ar", "ara", NULL,  "arabic",     "العربية"},
    {"tr", "tur", NULL,  "turkish",    "türkçe"},
    {"sv", "swe", NULL,  "swedish",    "svenska"},
    {"da", "dan", NULL,  "danish",     "dansk"},
    {"no", "nor", "nob", "norwegian",  "norsk"},
    {"fi", "fin", NULL,  "finnish",    "suomi"},
    {"el", "ell", "gre", "greek",      "ελληνικά"},
    {"cs", "ces", "cze", "czech",      "čeština"},
    {"hu", "hun", NULL,  "hungarian",  "magyar"},
    {"ro", "ron", "rum", "romanian",   "română"},
    {"he", "heb", NULL,  "hebrew",     "עברית"},
    {"hi", "hin", NULL,  "hindi",      "हिन्दी"},
    {"th", "tha", NULL,  "thai",       "ไทย"},
    {"vi", "vie", NULL,  "vietnamese", "tiếng việt"},
    {"id", "ind", NULL,  "indonesian", "bahasa indonesia"},
    {"uk", "ukr", NULL,  "ukrainian",  "українська"},
    {"hr", "hrv", NULL,  "croatian",   "hrvatski"},
    {"bg", "bul", NULL,  "bulgarian",  "български"},
};

int evo_lang_normalize(const char *tag, char out[4])
{
    if (out)
        out[0] = 0;
    if (!tag || !tag[0] || !out)
        return 0;

    /* lower-case copy; ASCII only - the native names above are matched
     * byte for byte, already lower case where the script has case */
    char low[48];
    size_t n = 0;
    while (n < sizeof low - 1 && tag[n]) {
        low[n] = (char)tolower((unsigned char)tag[n]);
        ++n;
    }
    low[n] = 0;
    while (n > 0 && (low[n - 1] == ' ' || low[n - 1] == '\t'))
        low[--n] = 0;

    /* "pt-BR", "en_US", "it.IT": the language part is what counts */
    char base[48];
    snprintf(base, sizeof base, "%s", low);
    for (size_t i = 0; base[i]; ++i) {
        if (base[i] == '-' || base[i] == '_' || base[i] == '.') {
            base[i] = 0;
            break;
        }
    }

    for (size_t i = 0; i < sizeof k_langs / sizeof k_langs[0]; ++i) {
        const struct lp_lang *l = &k_langs[i];
        if (!strcmp(base, l->a2) || !strcmp(base, l->a3) ||
            (l->alt && !strcmp(base, l->alt)) ||
            !strcmp(low, l->en) || !strcmp(low, l->native) ||
            !strcmp(base, l->en) || !strcmp(base, l->native)) {
            snprintf(out, 4, "%s", l->a3);
            return 1;
        }
    }
    return 0;
}

int evo_lang_is(const char *tag, const char *a3)
{
    if (!a3 || !a3[0])
        return 0;
    char c[4];
    return evo_lang_normalize(tag, c) && !strcmp(c, a3);
}

static void lp_store(char dst[4], const char *code)
{
    char c[4];
    if (code && code[0] && evo_lang_normalize(code, c))
        snprintf(dst, 4, "%s", c);
    else
        dst[0] = 0;
}

void evo_langpref_set_audio(const char *code)    { lp_store(g_audio, code); }
void evo_langpref_set_subtitle(const char *code) { lp_store(g_subtitle, code); }
const char *evo_langpref_audio(void)    { return g_audio; }
const char *evo_langpref_subtitle(void) { return g_subtitle; }

int evo_langpref_rank(const char *tag, char kind)
{
    const char *pref = (kind == 'a') ? g_audio : g_subtitle;
    char c[4];
    if (!evo_lang_normalize(tag, c))
        return 0;
    if (pref[0]) {
        if (!strcmp(c, pref))
            return 2;
        return !strcmp(c, "eng") ? 1 : 0;
    }
    /* Auto: subtitles keep the old English preference; audio has none */
    if (kind == 's' && !strcmp(c, "eng"))
        return 2;
    return 0;
}
