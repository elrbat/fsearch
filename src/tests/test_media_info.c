/*
 * Tests for the media info cache (fsearch_media_info.h): duration formatting, video extension
 * detection, reading files and the mtime/size based cache invalidation.
 */

#include "fsearch_media_info.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>

static char *tmp_dir = NULL;

static void
test_format_duration(void) {
    g_autofree char *a = fsearch_media_info_format_duration(-1);
    g_autofree char *b = fsearch_media_info_format_duration(0);
    g_autofree char *c = fsearch_media_info_format_duration(5000);
    g_autofree char *d = fsearch_media_info_format_duration(65000);
    g_autofree char *e = fsearch_media_info_format_duration(3700000);
    g_autofree char *f = fsearch_media_info_format_duration(59500); // rounds to the nearest second
    g_assert_cmpstr(a, ==, "");
    g_assert_cmpstr(b, ==, "0:00");
    g_assert_cmpstr(c, ==, "0:05");
    g_assert_cmpstr(d, ==, "1:05");
    g_assert_cmpstr(e, ==, "1:01:40");
    g_assert_cmpstr(f, ==, "1:00");
}

static void
test_is_video_extension(void) {
    g_assert_true(fsearch_media_info_is_video_extension("mp4"));
    g_assert_true(fsearch_media_info_is_video_extension("MKV"));
    g_assert_true(fsearch_media_info_is_video_extension("Avi"));
    g_assert_false(fsearch_media_info_is_video_extension("txt"));
    g_assert_false(fsearch_media_info_is_video_extension("mp3"));
    g_assert_false(fsearch_media_info_is_video_extension(""));
    g_assert_false(fsearch_media_info_is_video_extension(NULL));
}

static void
test_unreadable_files_are_cached_as_unknown(void) {
    g_autofree char *junk = g_build_filename(tmp_dir, "junk.mp4", NULL);
    g_assert_true(g_file_set_contents(junk, "this is not a video", -1, NULL));
    g_autofree char *missing = g_build_filename(tmp_dir, "missing.mp4", NULL);

    FsearchMediaInfo info;
    g_assert_false(fsearch_media_info_lookup(junk, 1, 19, &info));

    FsearchMediaJob jobs[] = {{.path = junk, .mtime = 1, .size = 19}, {.path = missing, .mtime = 1, .size = 0}};
    fsearch_media_info_probe_jobs(jobs, G_N_ELEMENTS(jobs), NULL);

    g_assert_true(fsearch_media_info_lookup(junk, 1, 19, &info));
    g_assert_cmpint(info.duration_ms, ==, -1);
    g_assert_true(fsearch_media_info_lookup(missing, 1, 0, &info));
    g_assert_cmpint(info.duration_ms, ==, -1);

    // A changed file (other mtime or size) must not be served from the cache
    g_assert_false(fsearch_media_info_lookup(junk, 2, 19, &info));
    g_assert_false(fsearch_media_info_lookup(junk, 1, 20, &info));
}

static void
test_cancelled_probe_caches_nothing(void) {
    g_autofree char *path = g_build_filename(tmp_dir, "cancelled.mp4", NULL);
    g_assert_true(g_file_set_contents(path, "x", -1, NULL));

    g_autoptr(GCancellable) cancellable = g_cancellable_new();
    g_cancellable_cancel(cancellable);
    FsearchMediaJob job = {.path = path, .mtime = 5, .size = 1};
    fsearch_media_info_probe_jobs(&job, 1, cancellable);

    FsearchMediaInfo info;
    g_assert_false(fsearch_media_info_lookup(path, 5, 1, &info));
}

static void
test_real_video_duration(void) {
    g_autofree char *ffmpeg = g_find_program_in_path("ffmpeg");
    if (!ffmpeg) {
        g_test_skip("ffmpeg is not installed, can't create a test video");
        return;
    }

    g_autofree char *path = g_build_filename(tmp_dir, "real.mp4", NULL);
    const char *argv[] = {ffmpeg, "-loglevel", "error", "-f", "lavfi", "-i", "testsrc=size=160x90:rate=1",
                          "-t",   "7",         "-pix_fmt", "yuv420p", "-c:v", "libx264", "-preset", "ultrafast", path, NULL};
    gint status = 0;
    if (!g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, NULL, &status, NULL)
        || !g_spawn_check_wait_status(status, NULL)) {
        g_test_skip("ffmpeg can't create the test video (libx264 missing?)");
        return;
    }

    GStatBuf st;
    g_assert_cmpint(g_stat(path, &st), ==, 0);
    FsearchMediaJob job = {.path = path, .mtime = st.st_mtime, .size = st.st_size};
    fsearch_media_info_probe_jobs(&job, 1, NULL);

    FsearchMediaInfo info;
    g_assert_true(fsearch_media_info_lookup(path, job.mtime, job.size, &info));
    g_assert_cmpint(info.duration_ms, ==, 7000);
    g_assert_cmpint(info.width, ==, 160);
    g_assert_cmpint(info.height, ==, 90);
    g_assert_cmpstr(info.video_codec, ==, "h264");
}

int
main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);

    // Keep the tests away from the real cache database of the user
    tmp_dir = g_dir_make_tmp("fsearch-media-info-XXXXXX", NULL);
    g_assert_nonnull(tmp_dir);
    g_setenv("XDG_DATA_HOME", tmp_dir, TRUE);

    g_test_add_func("/FSearch/media_info/format_duration", test_format_duration);
    g_test_add_func("/FSearch/media_info/is_video_extension", test_is_video_extension);
    g_test_add_func("/FSearch/media_info/unreadable_files_cached_as_unknown", test_unreadable_files_are_cached_as_unknown);
    g_test_add_func("/FSearch/media_info/cancelled_probe_caches_nothing", test_cancelled_probe_caches_nothing);
    g_test_add_func("/FSearch/media_info/real_video_duration", test_real_video_duration);

    return g_test_run();
}
