#include "fsearch_database_entry.h"
#include "fsearch_smart_match.h"
#include <glib.h>
#include <src/fsearch_query.h>
#include <string.h>

static void
assert_rewrite(const char *input, const char *expected) {
    g_autofree char *got = fsearch_smart_match_rewrite(input);
    if (g_strcmp0(got, expected) != 0) {
        g_printerr("rewrite of [%s]: expected [%s] but got [%s]\n", input, expected, got);
    }
    g_assert_cmpstr(got, ==, expected);
}

static void
test_rewrite_leaves_special_syntax_alone(void) {
    assert_rewrite("", "");
    assert_rewrite("foo", "foo");
    assert_rewrite("foo bar", "foo bar");
    assert_rewrite("ext:mp4", "ext:mp4");
    assert_rewrite("duration:>=30m res:fhd", "duration:>=30m res:fhd");
    assert_rewrite("*.mp4", "*.mp4");
    assert_rewrite("a/b.c", "a/b.c");
    assert_rewrite("regex:a.b", "regex:a.b");
    assert_rewrite("case:Foo.Bar", "case:Foo.Bar");
    assert_rewrite("exact: a.b", "exact: a.b");              // the term after a modifier is not touched
    assert_rewrite("path: my.folder", "path: my.folder");
    assert_rewrite("(a.b | c)", "(a.b | c)");                // brackets are left alone
    assert_rewrite("a\\.b", "a\\.b");                        // escapes are the user's business
    assert_rewrite("\"unterminated a.b", "\"unterminated a.b");
    assert_rewrite("movie -  name", "movie -  name");        // a lone dash is no term
    assert_rewrite("&& ||", "&& ||");
}

static void
test_rewrite_separators(void) {
    assert_rewrite("movie.name", "regex:\"movie[ ._-]+name\"");
    assert_rewrite("movie_name_2020", "regex:\"movie[ ._-]+name[ ._-]+[2٢۲][0٠۰][2٢۲][0٠۰]\"");
    assert_rewrite("\"movie name\"", "regex:\"movie[ ._-]+name\"");
    assert_rewrite("\"Movie - Name\"", "regex:\"Movie[ ._-]+Name\"");   // a run of separators is one separator
    assert_rewrite("!a.b", "!regex:\"a[ ._-]+b\"");
    assert_rewrite("a.b || c.d", "regex:\"a[ ._-]+b\" || regex:\"c[ ._-]+d\"");
    assert_rewrite("c++.mp4", "regex:\"c[+][+][ ._-]+mp[4٤۴]\"");   // digits match the Arabic-Indic and Persian ones
    assert_rewrite("ext:mkv movie.name", "ext:mkv regex:\"movie[ ._-]+name\"");
}

static void
test_rewrite_arabic(void) {
    // alef variants, hamza and optional diacritics after every letter
    g_autofree char *r = fsearch_smart_match_rewrite("احمد");
    g_assert_true(g_str_has_prefix(r, "regex:\"[اأإآٱ][ً-ٰٟـ]*ح"));
    g_assert_true(g_str_has_suffix(r, "\""));
    // plain ASCII digits in a term without separators or Arabic letters stay as they are
    assert_rewrite("2020", "2020");
    // but Arabic-Indic digits are rewritten so that they find the ASCII ones
    assert_rewrite("٢٠٢٠", "regex:\"[2٢۲][0٠۰][2٢۲][0٠۰]\"");
}

typedef struct {
    const char *name;
} TestFile;

static const char *names[] = {
    "Movie.Name.2020.mkv", "movie_name_2020.mp4", "Movie Name 2020.avi", "movie-name.mp4", "moviename.mp4", "MovieNameX.mp4",
    "احمد.mp4", "أحمد.mp4", "إحمد_2020.mp4", "أَحْمَد.mp4", "فاطمة.mp4", "فاطمه.mp4",
    "فيديو ٢٠٢٠.mp4", "فيديو 2020.mp4", "مؤمن.mp4", "مومن.mp4", "يحيى.mp4", "يحيي.mp4", "كتاب.pdf", "کتاب.pdf",
};

static char *
matches(const char *text, gboolean smart) {
    g_autofree char *rewritten = smart ? fsearch_smart_match_rewrite(text) : g_strdup(text);
    FsearchFilterManager *manager = fsearch_filter_manager_new_with_defaults();
    FsearchQuery *q = fsearch_query_new(rewritten, NULL, manager, 0, "test");
    FsearchQueryMatchData *match_data = fsearch_query_match_data_new(NULL, NULL);

    FsearchDatabaseEntry *root = db_entry_new(DATABASE_INDEX_PROPERTY_FLAG_NONE, "", NULL, DATABASE_ENTRY_TYPE_FOLDER);
    GString *result = g_string_new(NULL);
    for (guint i = 0; i < G_N_ELEMENTS(names); ++i) {
        FsearchDatabaseEntry *e = db_entry_new(DATABASE_INDEX_PROPERTY_FLAG_NONE, names[i], root, DATABASE_ENTRY_TYPE_FILE);
        fsearch_query_match_data_set_entry(match_data, e);
        if (fsearch_query_match(q, match_data)) {
            if (result->len) {
                g_string_append_c(result, '|');
            }
            g_string_append(result, names[i]);
        }
        db_entry_free(e);
    }
    db_entry_free(root);
    fsearch_query_match_data_free(match_data);
    fsearch_query_unref(q);
    fsearch_filter_manager_unref(manager);
    return g_string_free(result, FALSE);
}

#define ASSERT_MATCHES(text, expected)                                                                    \
    do {                                                                                                  \
        g_autofree char *got = matches(text, TRUE);                                                       \
        if (g_strcmp0(got, expected) != 0) {                                                              \
            g_printerr("query [%s]: expected [%s]\n                      but got [%s]\n", text, expected, got); \
        }                                                                                                 \
        g_assert_cmpstr(got, ==, expected);                                                               \
    } while (0)

static void
test_match_separators(void) {
    ASSERT_MATCHES("movie.name", "Movie.Name.2020.mkv|movie_name_2020.mp4|Movie Name 2020.avi|movie-name.mp4");
    ASSERT_MATCHES("movie_name", "Movie.Name.2020.mkv|movie_name_2020.mp4|Movie Name 2020.avi|movie-name.mp4");
    ASSERT_MATCHES("\"movie name 2020\"", "Movie.Name.2020.mkv|movie_name_2020.mp4|Movie Name 2020.avi");
    ASSERT_MATCHES("movie-name-2020", "Movie.Name.2020.mkv|movie_name_2020.mp4|Movie Name 2020.avi");
    // without smart matching a typed dot is literal
    g_autofree char *plain = matches("movie.name", FALSE);
    g_assert_cmpstr(plain, ==, "Movie.Name.2020.mkv");
    // combined with other terms, negation and OR
    ASSERT_MATCHES("movie.name ext:mkv", "Movie.Name.2020.mkv");
    ASSERT_MATCHES("movie.name !2020", "movie-name.mp4");
}

static void
test_match_arabic(void) {
    ASSERT_MATCHES("احمد", "احمد.mp4|أحمد.mp4|إحمد_2020.mp4|أَحْمَد.mp4");
    ASSERT_MATCHES("أحمد", "احمد.mp4|أحمد.mp4|إحمد_2020.mp4|أَحْمَد.mp4");
    ASSERT_MATCHES("أَحْمَد", "احمد.mp4|أحمد.mp4|إحمد_2020.mp4|أَحْمَد.mp4");
    ASSERT_MATCHES("فاطمه", "فاطمة.mp4|فاطمه.mp4");
    ASSERT_MATCHES("فاطمة", "فاطمة.mp4|فاطمه.mp4");
    ASSERT_MATCHES("يحيى", "يحيى.mp4|يحيي.mp4");
    ASSERT_MATCHES("مؤمن", "مؤمن.mp4|مومن.mp4");
    ASSERT_MATCHES("كتاب", "كتاب.pdf|کتاب.pdf");
    ASSERT_MATCHES("\"فيديو 2020\"", "فيديو ٢٠٢٠.mp4|فيديو 2020.mp4");
    ASSERT_MATCHES("\"فيديو ٢٠٢٠\"", "فيديو ٢٠٢٠.mp4|فيديو 2020.mp4");
    ASSERT_MATCHES("فيديو ٢٠٢٠", "فيديو ٢٠٢٠.mp4|فيديو 2020.mp4");
    // without smart matching only the exact spelling is found
    g_autofree char *plain = matches("احمد", FALSE);
    g_assert_cmpstr(plain, ==, "احمد.mp4");
}

int
main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/FSearch/smart_match/rewrite_leaves_special_syntax_alone", test_rewrite_leaves_special_syntax_alone);
    g_test_add_func("/FSearch/smart_match/rewrite_separators", test_rewrite_separators);
    g_test_add_func("/FSearch/smart_match/rewrite_arabic", test_rewrite_arabic);
    g_test_add_func("/FSearch/smart_match/match_separators", test_match_separators);
    g_test_add_func("/FSearch/smart_match/match_arabic", test_match_arabic);
    return g_test_run();
}
