/*
 * Tests for the search functions which work on properties inside video files:
 * duration:, res:, width:, height:, fps:, bitrate:, codec:, media: and pathlen:
 */

#include "fsearch_database_entry.h"
#include "fsearch_media_info.h"
#include <glib.h>
#include <glib/gstdio.h>
#include <src/fsearch_query.h>
#include <string.h>
#include <sys/stat.h>

typedef struct {
    const char *name;
    const char *size;       // ffmpeg testsrc size
    const char *rate;       // frame rate
    int seconds;
    const char *codec_args; // encoder
} TestVideo;

static const TestVideo videos[] = {
    {"short.mp4", "160x90", "30", 5, "libx264"},
    {"medium.mkv", "1280x720", "24000/1001", 65, "libx264"},
    {"scope.mp4", "1920x800", "25", 4, "mpeg4"},
};

static char *tmp_dir = NULL;
static FsearchDatabaseEntry *root = NULL;
static FsearchDatabaseEntry *dir_entry = NULL;
static GPtrArray *entries = NULL; // all file entries, in the order of `files` below
static const char *files[] = {"short.mp4", "medium.mkv", "scope.mp4", "broken.mp4", "pending.mp4", "notes.txt"};

static FsearchDatabaseEntry *
new_file_entry(const char *name) {
    g_autofree char *path = g_build_filename(tmp_dir, name, NULL);
    GStatBuf st;
    g_assert_cmpint(g_stat(path, &st), ==, 0);
    FsearchDatabaseIndexPropertyFlags flags =
        DATABASE_INDEX_PROPERTY_FLAG_SIZE | DATABASE_INDEX_PROPERTY_FLAG_MODIFICATION_TIME;
    return db_entry_new_with_attributes(flags,
                                        name,
                                        dir_entry,
                                        DATABASE_ENTRY_TYPE_FILE,
                                        DATABASE_INDEX_PROPERTY_SIZE,
                                        (int64_t)st.st_size,
                                        DATABASE_INDEX_PROPERTY_MODIFICATION_TIME,
                                        (int64_t)st.st_mtime,
                                        DATABASE_INDEX_PROPERTY_NONE);
}

static bool
create_test_files(void) {
    g_autofree char *ffmpeg = g_find_program_in_path("ffmpeg");
    if (!ffmpeg) {
        return false;
    }
    for (guint i = 0; i < G_N_ELEMENTS(videos); ++i) {
        g_autofree char *path = g_build_filename(tmp_dir, videos[i].name, NULL);
        g_autofree char *src = g_strdup_printf("testsrc=size=%s:rate=%s", videos[i].size, videos[i].rate);
        g_autofree char *secs = g_strdup_printf("%d", videos[i].seconds);
        const char *argv[] = {ffmpeg, "-loglevel", "error", "-f", "lavfi", "-i", src, "-t", secs, "-pix_fmt", "yuv420p",
                              "-c:v", videos[i].codec_args, "-preset", "ultrafast", path, NULL};
        // "-preset" is only understood by libx264, mpeg4 ignores the option with a warning
        gint status = 0;
        if (!g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, NULL, &status, NULL)
            || !g_spawn_check_wait_status(status, NULL)) {
            return false;
        }
    }
    g_autofree char *broken = g_build_filename(tmp_dir, "broken.mp4", NULL);
    g_autofree char *pending = g_build_filename(tmp_dir, "pending.mp4", NULL);
    g_autofree char *notes = g_build_filename(tmp_dir, "notes.txt", NULL);
    return g_file_set_contents(broken, "not a video", -1, NULL) && g_file_set_contents(pending, "not read yet", -1, NULL)
           && g_file_set_contents(notes, "text", -1, NULL);
}

static bool
setup(void) {
    if (entries) {
        return true;
    }
    if (!create_test_files()) {
        return false;
    }

    // Build the folder chain /<tmp_dir path elements>
    g_auto(GStrv) parts = g_strsplit(tmp_dir + 1, "/", -1);
    FsearchDatabaseIndexPropertyFlags flags =
        DATABASE_INDEX_PROPERTY_FLAG_SIZE | DATABASE_INDEX_PROPERTY_FLAG_MODIFICATION_TIME;
    root = db_entry_new(flags, "", NULL, DATABASE_ENTRY_TYPE_FOLDER);
    dir_entry = root;
    for (guint i = 0; parts[i]; ++i) {
        dir_entry = db_entry_new(flags, parts[i], dir_entry, DATABASE_ENTRY_TYPE_FOLDER);
    }

    entries = g_ptr_array_new();
    for (guint i = 0; i < G_N_ELEMENTS(files); ++i) {
        g_ptr_array_add(entries, new_file_entry(files[i]));
    }

    // Read the media info of everything except pending.mp4 and the text file
    FsearchMediaJob jobs[4];
    for (guint i = 0; i < 4; ++i) {
        FsearchDatabaseEntry *e = g_ptr_array_index(entries, i);
        g_autoptr(GString) path = db_entry_get_path_full(e);
        jobs[i].path = g_strdup(path->str);
        jobs[i].mtime = (int64_t)db_entry_get_mtime(e);
        jobs[i].size = (int64_t)db_entry_get_size(e);
    }
    fsearch_media_info_probe_jobs(jobs, 4, NULL);
    for (guint i = 0; i < 4; ++i) {
        g_free(jobs[i].path);
    }
    return true;
}

// Returns a string with the names of all test files matched by the query, e.g. "short.mp4 medium.mkv"
static char *
matches(const char *needle) {
    FsearchFilterManager *manager = fsearch_filter_manager_new_with_defaults();
    FsearchQuery *q = fsearch_query_new(needle, NULL, manager, 0, "debug_query");
    FsearchQueryMatchData *match_data = fsearch_query_match_data_new(NULL, NULL);

    GString *result = g_string_new(NULL);
    for (guint i = 0; i < entries->len; ++i) {
        FsearchDatabaseEntry *e = g_ptr_array_index(entries, i);
        fsearch_query_match_data_set_entry(match_data, e);
        if (fsearch_query_match(q, match_data)) {
            if (result->len) {
                g_string_append_c(result, ' ');
            }
            g_string_append(result, files[i]);
        }
    }
    fsearch_query_match_data_free(match_data);
    fsearch_query_unref(q);
    fsearch_filter_manager_unref(manager);
    return g_string_free(result, FALSE);
}

#define ASSERT_MATCHES(needle, expected)                                                                       \
    do {                                                                                                       \
        g_autofree char *got = matches(needle);                                                                \
        if (g_strcmp0(got, expected) != 0) {                                                                   \
            g_printerr("query [%s]: expected [%s] but got [%s]\n", needle, expected, got);                    \
        }                                                                                                      \
        g_assert_cmpstr(got, ==, expected);                                                                    \
    } while (0)

static void
test_duration(void) {
    if (!setup()) {
        g_test_skip("ffmpeg (with libx264) is needed to create test videos");
        return;
    }
    ASSERT_MATCHES("duration:5s", "short.mp4");
    ASSERT_MATCHES("duration:65s", "medium.mkv");
    ASSERT_MATCHES("duration:1m", "medium.mkv");   // "1m" covers 1:00 to 1:59
    ASSERT_MATCHES("duration:>1m", "medium.mkv");
    ASSERT_MATCHES("duration:<1m", "short.mp4 scope.mp4");
    ASSERT_MATCHES("duration:>=4 duration:<=5", "short.mp4 scope.mp4");
    ASSERT_MATCHES("duration:\"1:05\"", "medium.mkv"); // clock notation needs quotes, ':' is a reserved character
    ASSERT_MATCHES("duration:4..10", "short.mp4 scope.mp4");
    ASSERT_MATCHES("duration:1m5s", "medium.mkv");
    ASSERT_MATCHES("duration:>1h", "");
    // invalid values never match anything
    ASSERT_MATCHES("duration:abc", "");
}

static void
test_resolution(void) {
    if (!setup()) {
        g_test_skip("ffmpeg (with libx264) is needed to create test videos");
        return;
    }
    ASSERT_MATCHES("res:720p", "medium.mkv");
    ASSERT_MATCHES("res:hd", "medium.mkv");
    ASSERT_MATCHES("res:1080p", "scope.mp4"); // 1920x800 is a 1080p film format
    ASSERT_MATCHES("res:fhd", "scope.mp4");
    ASSERT_MATCHES("res:>=720p", "medium.mkv scope.mp4");
    ASSERT_MATCHES("res:sd", "short.mp4");
    ASSERT_MATCHES("res:<720", "short.mp4");
    ASSERT_MATCHES("res:4k", "");
    ASSERT_MATCHES("width:1920", "scope.mp4");
    ASSERT_MATCHES("width:>=1280", "medium.mkv scope.mp4");
    ASSERT_MATCHES("height:90", "short.mp4");
    ASSERT_MATCHES("res:720p..1080p", "medium.mkv");
}

static void
test_fps_codec_bitrate(void) {
    if (!setup()) {
        g_test_skip("ffmpeg (with libx264) is needed to create test videos");
        return;
    }
    ASSERT_MATCHES("fps:30", "short.mp4");
    ASSERT_MATCHES("fps:24", "medium.mkv");       // 23.976 counts as 24
    ASSERT_MATCHES("fps:23.976", "medium.mkv");
    ASSERT_MATCHES("fps:>=25", "short.mp4 scope.mp4");
    ASSERT_MATCHES("fps:<25", "medium.mkv");
    ASSERT_MATCHES("codec:h264", "short.mp4 medium.mkv");
    ASSERT_MATCHES("codec:x264", "short.mp4 medium.mkv"); // alias
    ASSERT_MATCHES("codec:mpeg4", "scope.mp4");
    ASSERT_MATCHES("codec:xvid", "scope.mp4");           // alias
    ASSERT_MATCHES("codec:h264;mpeg4", "short.mp4 medium.mkv scope.mp4");
    ASSERT_MATCHES("codec:hevc", "");
    // short.mp4 ~81 kb/s, medium.mkv ~258 kb/s (the file has no bit rate in its header, so it is the
    // average over the whole file), scope.mp4 ~1095 kb/s
    ASSERT_MATCHES("bitrate:>1m", "scope.mp4");
    ASSERT_MATCHES("bitrate:>1000 bitrate:<2000", "scope.mp4");
    ASSERT_MATCHES("bitrate:<100", "short.mp4");
    ASSERT_MATCHES("bitrate:100..500", "medium.mkv");
    ASSERT_MATCHES("bitrate:>0.5mbps", "scope.mp4");
}

static void
test_media_status_and_pathlen(void) {
    if (!setup()) {
        g_test_skip("ffmpeg (with libx264) is needed to create test videos");
        return;
    }
    ASSERT_MATCHES("media:ok", "short.mp4 medium.mkv scope.mp4");
    ASSERT_MATCHES("media:unreadable", "broken.mp4");
    ASSERT_MATCHES("media:corrupt", "broken.mp4");
    ASSERT_MATCHES("media:pending", "pending.mp4"); // not read yet, so no other media filter can match it
    ASSERT_MATCHES("duration:>0", "short.mp4 medium.mkv scope.mp4");

    // pathlen works for every kind of file
    const guint dir_len = (guint)g_utf8_strlen(tmp_dir, -1) + 1; // + separator
    g_autofree char *q1 = g_strdup_printf("pathlen:%u", dir_len + (guint)strlen("notes.txt"));
    ASSERT_MATCHES(q1, "short.mp4 scope.mp4 notes.txt"); // all names with 9 characters
    g_autofree char *q2 = g_strdup_printf("pathlen:>%u", dir_len + 9);
    ASSERT_MATCHES(q2, "medium.mkv broken.mp4 pending.mp4");
}

int
main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);

    tmp_dir = g_dir_make_tmp("fsearch-query-media-XXXXXX", NULL);
    g_assert_nonnull(tmp_dir);
    // Keep the tests away from the real media info cache of the user
    g_autofree char *data_home = g_build_filename(tmp_dir, "xdg-data", NULL);
    g_setenv("XDG_DATA_HOME", data_home, TRUE);

    g_test_add_func("/FSearch/query_media/duration", test_duration);
    g_test_add_func("/FSearch/query_media/resolution", test_resolution);
    g_test_add_func("/FSearch/query_media/fps_codec_bitrate", test_fps_codec_bitrate);
    g_test_add_func("/FSearch/query_media/status_and_pathlen", test_media_status_and_pathlen);

    return g_test_run();
}
