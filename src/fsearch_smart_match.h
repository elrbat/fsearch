#pragma once

#include <glib.h>

G_BEGIN_DECLS

// "Smart matching" makes plain search terms tolerant against the many ways file names are written:
//
//  * Separators: `.`, `_`, `-` and spaces are interchangeable, so "movie name 2020" finds
//    "Movie.Name.2020", "movie_name_2020" and "Movie - Name 2020".
//  * Arabic letters: alef with and without hamza (ا أ إ آ), ه/ة, ي/ى/ی, ك/ک and و/ؤ are treated as
//    equal, diacritics (tashkeel) and tatweel are ignored, and Arabic-Indic (٠١٢) and Persian (۰۱۲)
//    digits match the ASCII digits (and the other way round).
//
// The function rewrites the terms of a search query which need this into regular expression terms
// (`regex:"..."`). Everything else - functions (`ext:mp4`), modifiers, operators, wildcards, paths,
// terms in front of a modifier like `exact:` and terms which already are special in any way - is
// left exactly as typed. Returns a newly allocated string (free with g_free); if nothing has to be
// rewritten it is a copy of the text.
char *
fsearch_smart_match_rewrite(const char *text);

G_END_DECLS
