#include "fsearch_filter_panel_query.h"
#include <glib.h>
#include <src/fsearch_query.h>
#include <string.h>

static char *
build(const FsearchFilterPanelValues *v, bool media, GPtrArray *problems) {
    return fsearch_filter_panel_build_query(v, media, problems);
}

static void
test_empty(void) {
    FsearchFilterPanelValues v = {0};
    g_assert_null(build(&v, true, NULL));
    FsearchFilterPanelValues blank = {.extensions = "  ", .size_min = "", .codec = " ,; "};
    g_assert_null(build(&blank, true, NULL));
    g_assert_false(fsearch_filter_panel_has_media_conditions(&v));
}

static void
test_basic_fields(void) {
    FsearchFilterPanelValues v = {.kind = 1, .extensions = "mp4, .mkv  *avi", .size_min = "100mb", .size_max = "2gb", .modified = "pastweek"};
    g_autofree char *q = build(&v, true, NULL);
    g_assert_cmpstr(q, ==, "files: ext:mp4;mkv;avi size:>=100mb size:<=2gb dm:pastweek");

    FsearchFilterPanelValues folders = {.kind = 2, .pathlen_min = "200"};
    g_autofree char *q2 = build(&folders, true, NULL);
    g_assert_cmpstr(q2, ==, "folders: pathlen:>=200");
}

static void
test_media_fields(void) {
    FsearchFilterPanelValues v = {.duration_min = "30m", .duration_max = "2h", .res_min = "hd", .res_max = "fhd", .codec = "h264, hevc",
                                  .fps_min = "24", .fps_max = "30", .bitrate_min = "2m"};
    g_autofree char *q = build(&v, true, NULL);
    g_assert_cmpstr(q, ==, "duration:>=30m duration:<=2h res:>=hd res:<=fhd fps:>=24 fps:<=30 bitrate:>=2m codec:h264;hevc");
    g_assert_true(fsearch_filter_panel_has_media_conditions(&v));

    // Without media conditions (used to find the videos which still have to be analyzed) nothing is left
    g_assert_null(build(&v, false, NULL));

    FsearchFilterPanelValues same = {.res_min = "fhd", .res_max = "1080p"};
    g_autofree char *q2 = build(&same, true, NULL);
    g_assert_cmpstr(q2, ==, "res:fhd");
}

static void
test_problems(void) {
    GPtrArray *problems = g_ptr_array_new_with_free_func(g_free);

    FsearchFilterPanelValues v = {.duration_min = "2h", .duration_max = "30m", .size_min = "abc", .fps_max = "-5", .res_min = "fhd", .res_max = "hd"};
    g_autofree char *q = build(&v, true, problems);
    g_assert_null(q); // nothing valid is left
    g_assert_cmpuint(problems->len, ==, 4);
    g_assert_nonnull(strstr(g_ptr_array_index(problems, 0), "Size"));
    g_assert_nonnull(strstr(g_ptr_array_index(problems, 0), "not a valid"));
    g_assert_nonnull(strstr(g_ptr_array_index(problems, 1), "Duration"));
    g_assert_nonnull(strstr(g_ptr_array_index(problems, 1), "larger than the maximum"));
    g_ptr_array_unref(problems);

    // valid fields are kept when others have problems
    problems = g_ptr_array_new_with_free_func(g_free);
    FsearchFilterPanelValues mixed = {.extensions = "mp4", .duration_min = "xyz"};
    g_autofree char *q2 = build(&mixed, true, problems);
    g_assert_cmpstr(q2, ==, "ext:mp4");
    g_assert_cmpuint(problems->len, ==, 1);
    g_ptr_array_unref(problems);
}

static void
test_finders(void) {
    FsearchFilterPanelValues v = {.finder = "unreadable"};
    g_autofree char *q = build(&v, true, NULL);
    g_assert_cmpstr(q, ==, "media:unreadable");
    g_assert_null(build(&v, false, NULL));
    g_assert_true(fsearch_filter_panel_has_media_conditions(&v));

    FsearchFilterPanelValues empty_files = {.finder = "empty_files", .extensions = "mp4"};
    g_autofree char *q2 = build(&empty_files, false, NULL);
    g_assert_cmpstr(q2, ==, "ext:mp4 files: size:0");
    g_assert_false(fsearch_filter_panel_has_media_conditions(&empty_files));

    FsearchFilterPanelValues unknown = {.finder = "does_not_exist"};
    g_assert_null(build(&unknown, true, NULL));
}

static void
test_every_finder_and_all_fields_parse(void) {
    // The generated text must be valid search syntax: every token must turn into a real query node
    // (invalid ones become a match-nothing node, which prints as "match_nothing")
    FsearchFilterPanelValues all = {.kind = 1, .extensions = "mp4", .size_min = "1mb", .size_max = "9gb", .modified = "pastyear",
                                    .duration_min = "1m", .duration_max = "3h", .res_min = "sd", .res_max = "uhd", .codec = "h264",
                                    .fps_min = "23.976", .fps_max = "60", .bitrate_min = "500", .bitrate_max = "20m",
                                    .pathlen_min = "10", .pathlen_max = "300"};
    g_autofree char *q = build(&all, true, NULL);
    g_assert_nonnull(q);
    g_assert_null(strstr(q, "  "));
    FsearchFilterManager *manager = fsearch_filter_manager_new_with_defaults();
    FsearchQuery *query = fsearch_query_new(q, NULL, manager, 0, "test");
    g_assert_nonnull(query);
    fsearch_query_unref(query);

    for (guint i = 0; i < fsearch_filter_panel_num_finders; ++i) {
        FsearchFilterPanelValues f = {.finder = fsearch_filter_panel_finders[i].id};
        g_autofree char *fq = build(&f, true, NULL);
        g_assert_cmpstr(fq, ==, fsearch_filter_panel_finders[i].query);
        FsearchQuery *fquery = fsearch_query_new(fq, NULL, manager, 0, "test");
        g_assert_nonnull(fquery);
        fsearch_query_unref(fquery);
    }
    fsearch_filter_manager_unref(manager);
}

static void
test_conditions(void) {
    FsearchFilterPanelValues v = {.kind = 1, .extensions = "mp4, mkv", .size_min = "100mb", .modified = "pastweek", .res_min = "hd", .res_max = "fhd", .codec = "h264", .finder = "unreadable"};
    GPtrArray *conditions = fsearch_filter_panel_build_conditions(&v, NULL);
    g_assert_cmpuint(conditions->len, ==, 7);

    const char *ids[] = {"type", "ext", "size", "modified", "res", "codec", "finder"};
    const char *labels[] = {"Files only", "Extension: mp4;mkv", "Size: ≥ 100mb", "Modified: Past week", "Resolution: hd – fhd", "Codec: h264", "Find problems: Videos which can't be read (corrupt?)"};
    for (guint i = 0; i < conditions->len; ++i) {
        const FsearchFilterPanelCondition *c = g_ptr_array_index(conditions, i);
        g_assert_cmpstr(c->id, ==, ids[i]);
        g_assert_cmpstr(c->label, ==, labels[i]);
    }

    g_autofree char *all = fsearch_filter_panel_conditions_to_query(conditions, true, NULL);
    g_assert_cmpstr(all, ==, "files: ext:mp4;mkv size:>=100mb dm:pastweek res:>=hd res:<=fhd codec:h264 media:unreadable");
    g_autofree char *no_codec = fsearch_filter_panel_conditions_to_query(conditions, true, "codec");
    g_assert_cmpstr(no_codec, ==, "files: ext:mp4;mkv size:>=100mb dm:pastweek res:>=hd res:<=fhd media:unreadable");
    g_autofree char *no_media = fsearch_filter_panel_conditions_to_query(conditions, false, NULL);
    g_assert_cmpstr(no_media, ==, "files: ext:mp4;mkv size:>=100mb dm:pastweek");
    g_autofree char *no_res = fsearch_filter_panel_conditions_to_query(conditions, true, "res");
    g_assert_nonnull(no_res);
    g_assert_null(strstr(no_res, "res:"));
    g_ptr_array_unref(conditions);

    FsearchFilterPanelValues none = {0};
    GPtrArray *empty = fsearch_filter_panel_build_conditions(&none, NULL);
    g_assert_cmpuint(empty->len, ==, 0);
    g_assert_null(fsearch_filter_panel_conditions_to_query(empty, true, NULL));
    g_ptr_array_unref(empty);
}

int
main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/FSearch/filter_panel_query/empty", test_empty);
    g_test_add_func("/FSearch/filter_panel_query/basic_fields", test_basic_fields);
    g_test_add_func("/FSearch/filter_panel_query/media_fields", test_media_fields);
    g_test_add_func("/FSearch/filter_panel_query/problems", test_problems);
    g_test_add_func("/FSearch/filter_panel_query/finders", test_finders);
    g_test_add_func("/FSearch/filter_panel_query/valid_syntax", test_every_finder_and_all_fields_parse);
    g_test_add_func("/FSearch/filter_panel_query/conditions", test_conditions);
    return g_test_run();
}
