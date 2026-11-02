/*
 * evo_langpref - the audio and subtitle languages the user prefers.
 *
 * Settings → Audio → AUDIO LANGUAGE and Settings → Subtitles → SUBTITLE
 * LANGUAGE store an ISO 639-2/T code here ("ita", "eng"; "" = Auto). The track
 * pickers that run when a file opens (audio stream choice, the embedded
 * subtitle choice, the online subtitles from Stremio addons) rank tracks with
 * evo_langpref_rank().
 *
 * Auto keeps EVO's old behaviour: audio is the first decodable track, and
 * subtitles prefer English.
 */
#ifndef EVO_LANGPREF_H
#define EVO_LANGPREF_H

#ifdef __cplusplus
extern "C" {
#endif

/* "" or NULL = Auto. Any code evo_lang_normalize() accepts. */
void evo_langpref_set_audio(const char *code);
void evo_langpref_set_subtitle(const char *code);

/* The stored 639-2/T code, "" for Auto. */
const char *evo_langpref_audio(void);
const char *evo_langpref_subtitle(void);

/*
 * Normalise a language tag to its ISO 639-2/T code: "it", "ita", "it-IT",
 * "Italian", "italiano" -> "ita"; "fre"/"fr"/"French" -> "fra"; "pob" and
 * "pt-BR" -> "por". `out` must hold 4 bytes. Returns 1 when the tag is a
 * known language, else 0 with out = "".
 */
int evo_lang_normalize(const char *tag, char out[4]);

/* 1 when `tag` names the same language as the 639-2/T code `a3`. */
int evo_lang_is(const char *tag, const char *a3);

/*
 * How well a track's language suits what the user wants, for `kind` 'a'
 * (audio) or 's' (subtitles):
 *   2  the preferred language (for subtitles with Auto: English)
 *   1  English, when something else is preferred (a usable fallback)
 *   0  anything else, or no language tag
 */
int evo_langpref_rank(const char *tag, char kind);

#ifdef __cplusplus
}
#endif

#endif
