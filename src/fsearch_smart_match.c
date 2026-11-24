#include "fsearch_smart_match.h"

#include <string.h>

#define SEPARATOR_CLASS "[ ._-]+"

static gboolean
is_separator(gunichar c) {
    return c == '.' || c == '_' || c == '-' || g_unichar_isspace(c);
}

static gboolean
is_tashkeel(gunichar c) {
    // fatha, damma, kasra, shadda, sukun, ..., superscript alef and tatweel
    return (c >= 0x064B && c <= 0x065F) || c == 0x0670 || c == 0x0640;
}

static gboolean
is_arabic_script(gunichar c) {
    return (c >= 0x0600 && c <= 0x06FF) || (c >= 0x0750 && c <= 0x077F);
}

static gboolean
is_arabic_digit(gunichar c) {
    return (c >= 0x0660 && c <= 0x0669) || (c >= 0x06F0 && c <= 0x06F9);
}

// Characters which may appear in a term that is rewritten. Everything else (operators, brackets,
// wildcards, colons, slashes, backslashes, quotes, ...) means that the term is left alone.
static gboolean
is_allowed_char(gunichar c) {
    if (g_unichar_isalnum(c) || g_unichar_ismark(c) || is_separator(c)) {
        return TRUE;
    }
    return c < 0x80 && strchr(",'&+$#@%", (int)c) != NULL;
}

static void
append_unichar(GString *s, gunichar c) {
    char buf[8];
    const int len = g_unichar_to_utf8(c, buf);
    g_string_append_len(s, buf, len);
}

static void
append_class(GString *s, const gunichar *chars) {
    g_string_append_c(s, '[');
    for (; *chars; ++chars) {
        append_unichar(s, *chars);
    }
    g_string_append_c(s, ']');
}

static void
append_optional_tashkeel(GString *s) {
    g_string_append_c(s, '[');
    append_unichar(s, 0x064B);
    g_string_append_c(s, '-');
    append_unichar(s, 0x065F);
    append_unichar(s, 0x0670);
    append_unichar(s, 0x0640);
    g_string_append(s, "]*");
}

// Appends the regular expression for one character: letters and digits become classes of their variants,
// letters of the Arabic script may be followed by diacritics.
static void
append_char_pattern(GString *pattern, gunichar c) {
    static const gunichar alef[] = {0x0627, 0x0623, 0x0625, 0x0622, 0x0671, 0};
    static const gunichar heh[] = {0x0647, 0x0629, 0};
    static const gunichar yeh[] = {0x064A, 0x0649, 0x06CC, 0x0626, 0};
    static const gunichar kaf[] = {0x0643, 0x06A9, 0};
    static const gunichar waw[] = {0x0648, 0x0624, 0};

    // Digits: ASCII, Arabic-Indic and Persian
    int digit = -1;
    if (c >= '0' && c <= '9') {
        digit = (int)(c - '0');
    }
    else if (c >= 0x0660 && c <= 0x0669) {
        digit = (int)(c - 0x0660);
    }
    else if (c >= 0x06F0 && c <= 0x06F9) {
        digit = (int)(c - 0x06F0);
    }
    if (digit >= 0) {
        const gunichar chars[] = {(gunichar)('0' + digit), (gunichar)(0x0660 + digit), (gunichar)(0x06F0 + digit), 0};
        append_class(pattern, chars);
        return;
    }

    switch (c) {
    case 0x0627:
    case 0x0623:
    case 0x0625:
    case 0x0622:
    case 0x0671:
        append_class(pattern, alef);
        break;
    case 0x0647:
    case 0x0629:
        append_class(pattern, heh);
        break;
    case 0x064A:
    case 0x0649:
    case 0x06CC:
    case 0x0626:
        append_class(pattern, yeh);
        break;
    case 0x0643:
    case 0x06A9:
        append_class(pattern, kaf);
        break;
    case 0x0648:
    case 0x0624:
        append_class(pattern, waw);
        break;
    case '+':
    case '$':
        // special characters of regular expressions, written as a class so that no backslash is needed
        g_string_append_c(pattern, '[');
        g_string_append_c(pattern, (char)c);
        g_string_append_c(pattern, ']');
        break;
    default:
        append_unichar(pattern, c);
        break;
    }

    if (is_arabic_script(c) && g_unichar_isalpha(c)) {
        append_optional_tashkeel(pattern);
    }
}

// Builds the regular expression for the content of a term
static char *
build_pattern(const char *content) {
    GString *pattern = g_string_new(NULL);
    gboolean last_was_separator = FALSE;
    for (const char *p = content; *p; p = g_utf8_next_char(p)) {
        const gunichar c = g_utf8_get_char(p);
        if (is_tashkeel(c)) {
            continue;
        }
        if (is_separator(c)) {
            if (!last_was_separator) {
                g_string_append(pattern, SEPARATOR_CLASS);
            }
            last_was_separator = TRUE;
            continue;
        }
        last_was_separator = FALSE;
        append_char_pattern(pattern, c);
    }
    return g_string_free(pattern, FALSE);
}

static gboolean
content_is_eligible(const char *content, gboolean *needs_rewrite_out) {
    gboolean needs_rewrite = FALSE;
    gboolean has_content = FALSE;
    if (!g_utf8_validate(content, -1, NULL)) {
        return FALSE;
    }
    for (const char *p = content; *p; p = g_utf8_next_char(p)) {
        const gunichar c = g_utf8_get_char(p);
        if (!is_allowed_char(c)) {
            return FALSE;
        }
        if (is_separator(c) || is_arabic_script(c) || is_arabic_digit(c)) {
            needs_rewrite = TRUE;
        }
        if (!is_separator(c) && !is_tashkeel(c)) {
            has_content = TRUE;
        }
    }
    *needs_rewrite_out = needs_rewrite;
    return has_content;
}

static void
append_rewritten_term(GString *out, const char *content, gboolean negated) {
    g_autofree char *pattern = build_pattern(content);
    if (negated) {
        g_string_append_c(out, '!');
    }
    g_string_append(out, "regex:\"");
    g_string_append(out, pattern);
    g_string_append_c(out, '"');
}

char *
fsearch_smart_match_rewrite(const char *text) {
    g_return_val_if_fail(text, NULL);

    GString *out = g_string_new(NULL);
    gboolean skip_next_term = FALSE;
    const char *p = text;

    while (*p) {
        if (g_ascii_isspace(*p)) {
            g_string_append_c(out, *p);
            p++;
            continue;
        }

        // Find the end of the token. Quoted parts belong to the token, even if they contain spaces.
        const char *start = p;
        gboolean in_quote = FALSE;
        gboolean has_backslash = FALSE;
        while (*p && (in_quote || !g_ascii_isspace(*p))) {
            if (*p == '\\') {
                has_backslash = TRUE;
                if (p[1]) {
                    p++;
                }
            }
            else if (*p == '"') {
                in_quote = !in_quote;
            }
            p++;
        }
        const size_t len = (size_t)(p - start);
        g_autofree char *token = g_strndup(start, len);

        const gboolean ends_with_colon = len > 0 && token[len - 1] == ':';
        gboolean rewritten = FALSE;

        if (!skip_next_term && !has_backslash && !ends_with_colon && !in_quote) {
            gboolean negated = FALSE;
            const char *body = token;
            if (body[0] == '!' && body[1]) {
                negated = TRUE;
                body++;
            }

            gboolean needs_rewrite = FALSE;
            if (body[0] == '"') {
                // A quoted phrase: the quote must close at the very end of the token
                const char *closing = strchr(body + 1, '"');
                if (closing && closing[1] == '\0') {
                    g_autofree char *content = g_strndup(body + 1, (gsize)(closing - body - 1));
                    if (content_is_eligible(content, &needs_rewrite) && needs_rewrite) {
                        append_rewritten_term(out, content, negated);
                        rewritten = TRUE;
                    }
                }
            }
            else if (content_is_eligible(body, &needs_rewrite) && needs_rewrite) {
                append_rewritten_term(out, body, negated);
                rewritten = TRUE;
            }
        }

        if (!rewritten) {
            g_string_append_len(out, start, len);
        }
        // "exact:" or "path:" without a word change how the next term is matched: don't touch that term
        skip_next_term = ends_with_colon;
    }

    return g_string_free(out, FALSE);
}
