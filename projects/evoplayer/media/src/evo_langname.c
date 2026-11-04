/*
 * evo_langname - a language's own name for its ISO 639 code. See evo_langname.h.
 *
 * Only languages whose script the UI can draw (baked Latin/Greek/Cyrillic,
 * Hebrew, Arabic, Thai, the Indic scripts, CJK) are listed.
 */
#include "evo_langname.h"

#include <ctype.h>
#include <string.h>

struct lang {
    const char *a2, *a3, *a3b, *name;
};

static const struct lang langs[] = {
    {"en", "eng", NULL, "English"},
    {"es", "spa", NULL, "Español"},
    {"fr", "fra", "fre", "Français"},
    {"de", "deu", "ger", "Deutsch"},
    {"it", "ita", NULL, "Italiano"},
    {"pt", "por", NULL, "Português"},
    {"nl", "nld", "dut", "Nederlands"},
    {"sv", "swe", NULL, "Svenska"},
    {"no", "nor", NULL, "Norsk"},
    {"nb", "nob", NULL, "Norsk bokmål"},
    {"da", "dan", NULL, "Dansk"},
    {"fi", "fin", NULL, "Suomi"},
    {"is", "isl", "ice", "Íslenska"},
    {"et", "est", NULL, "Eesti"},
    {"lv", "lav", NULL, "Latviešu"},
    {"lt", "lit", NULL, "Lietuvių"},
    {"pl", "pol", NULL, "Polski"},
    {"cs", "ces", "cze", "Čeština"},
    {"sk", "slk", "slo", "Slovenčina"},
    {"hu", "hun", NULL, "Magyar"},
    {"ro", "ron", "rum", "Română"},
    {"hr", "hrv", NULL, "Hrvatski"},
    {"sl", "slv", NULL, "Slovenščina"},
    {"sq", "sqi", "alb", "Shqip"},
    {"ca", "cat", NULL, "Català"},
    {"gl", "glg", NULL, "Galego"},
    {"eu", "eus", "baq", "Euskara"},
    {"tr", "tur", NULL, "Türkçe"},
    {"vi", "vie", NULL, "Tiếng Việt"},
    {"id", "ind", NULL, "Bahasa Indonesia"},
    {"ms", "msa", "may", "Bahasa Melayu"},
    {"ru", "rus", NULL, "Русский"},
    {"uk", "ukr", NULL, "Українська"},
    {"bg", "bul", NULL, "Български"},
    {"sr", "srp", NULL, "Српски"},
    {"mk", "mkd", "mac", "Македонски"},
    {"be", "bel", NULL, "Беларуская"},
    {"el", "ell", "gre", "Ελληνικά"},
    {"he", "heb", NULL, "עברית"},
    {"ar", "ara", NULL, "العربية"},
    {"fa", "fas", "per", "فارسی"},
    {"ur", "urd", NULL, "اردو"},
    {"hi", "hin", NULL, "हिन्दी"},
    {"mr", "mar", NULL, "मराठी"},
    {"ne", "nep", NULL, "नेपाली"},
    {"bn", "ben", NULL, "বাংলা"},
    {"ta", "tam", NULL, "தமிழ்"},
    {"te", "tel", NULL, "తెలుగు"},
    {"ml", "mal", NULL, "മലയാളം"},
    {"th", "tha", NULL, "ไทย"},
    {"zh", "zho", "chi", "中文"},
    {"ja", "jpn", NULL, "日本語"},
    {"ko", "kor", NULL, "한국어"},
};

const char *evo_language_native_name(const char *code)
{
    if (!code || !code[0])
        return NULL;
    char low[8];
    size_t n = 0;
    for (; n < sizeof low - 1 && code[n]; ++n)
        low[n] = (char)tolower((unsigned char)code[n]);
    low[n] = 0;
    if (code[n] != 0 && code[n] != '-' && code[n] != '_')
        return NULL; /* longer than a language code */
    /* "pt-BR": the language part is what counts. */
    for (size_t i = 0; i < n; ++i)
        if (low[i] == '-' || low[i] == '_') {
            low[i] = 0;
            break;
        }
    for (size_t i = 0; i < sizeof langs / sizeof langs[0]; ++i) {
        const struct lang *l = &langs[i];
        if (strcmp(low, l->a2) == 0 || strcmp(low, l->a3) == 0 || (l->a3b && strcmp(low, l->a3b) == 0))
            return l->name;
    }
    return NULL;
}
