#define G_LOG_DOMAIN "fsearch-media-info"

#include "fsearch_media_info.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/log.h>
#include <sqlite3.h>
#include <string.h>

#define PROBE_TIMEOUT_US (20 * G_USEC_PER_SEC)
#define MAX_PROBE_THREADS 8
#define MAX_ASYNC_THREADS 4
#define MEDIA_CACHE_SCHEMA_VERSION 2

typedef struct {
    int64_t mtime;
    int64_t size;
    FsearchMediaInfo info;
} CacheEntry;

static GRWLock cache_lock;
static GHashTable *cache = NULL; // char *path -> CacheEntry *
static sqlite3 *cache_db = NULL;
static sqlite3_stmt *insert_stmt = NULL;
static gsize cache_initialized = 0;

static const char *video_extensions[] = {
    "mp4", "mkv", "avi", "mov", "wmv", "flv", "webm", "m4v", "mpg", "mpeg", "ts", "m2ts", "mts", "vob",
    "3gp", "ogv", "divx", "rm",  "rmvb", "asf", "mxf", "f4v", "m2v", "mp2", "qt",
};

bool
fsearch_media_info_is_video_extension(const char *extension) {
    if (!extension || !*extension) {
        return false;
    }
    for (guint i = 0; i < G_N_ELEMENTS(video_extensions); ++i) {
        if (!g_ascii_strcasecmp(extension, video_extensions[i])) {
            return true;
        }
    }
    return false;
}

static void
cache_entry_free(gpointer data) {
    g_free(data);
}

// Must be called with the cache lock held for writing
static void
cache_load_from_db(void) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(cache_db,
                           "SELECT path, mtime, size, duration_ms, width, height, bit_rate, video_codec, frame_rate FROM media",
                           -1,
                           &stmt,
                           NULL)
        != SQLITE_OK) {
        return;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *path = (const char *)sqlite3_column_text(stmt, 0);
        if (!path) {
            continue;
        }
        CacheEntry *entry = g_new0(CacheEntry, 1);
        entry->mtime = sqlite3_column_int64(stmt, 1);
        entry->size = sqlite3_column_int64(stmt, 2);
        entry->info.duration_ms = sqlite3_column_int64(stmt, 3);
        entry->info.width = sqlite3_column_int(stmt, 4);
        entry->info.height = sqlite3_column_int(stmt, 5);
        entry->info.bit_rate = sqlite3_column_int64(stmt, 6);
        const char *codec = (const char *)sqlite3_column_text(stmt, 7);
        if (codec) {
            g_strlcpy(entry->info.video_codec, codec, sizeof(entry->info.video_codec));
        }
        entry->info.frame_rate = sqlite3_column_double(stmt, 8);
        g_hash_table_replace(cache, g_strdup(path), entry);
    }
    sqlite3_finalize(stmt);
}

// Must be called with the cache lock held for writing
static void
cache_init_body(void) {
    {
        av_log_set_level(AV_LOG_QUIET);
        cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, cache_entry_free);

        g_autofree char *dir = g_build_filename(g_get_user_data_dir(), "fsearch", NULL);
        g_mkdir_with_parents(dir, 0700);
        g_autofree char *db_path = g_build_filename(dir, "media_info.db", NULL);

        if (sqlite3_open(db_path, &cache_db) == SQLITE_OK) {
            sqlite3_exec(cache_db, "PRAGMA synchronous=OFF", NULL, NULL, NULL);
            sqlite3_exec(cache_db, "PRAGMA journal_mode=MEMORY", NULL, NULL, NULL);
            // The cache only holds data which can be read from the files again, so if the layout of the
            // table changed it is simply thrown away and filled again on demand.
            int schema_version = 0;
            sqlite3_stmt *version_stmt = NULL;
            if (sqlite3_prepare_v2(cache_db, "PRAGMA user_version", -1, &version_stmt, NULL) == SQLITE_OK) {
                if (sqlite3_step(version_stmt) == SQLITE_ROW) {
                    schema_version = sqlite3_column_int(version_stmt, 0);
                }
                sqlite3_finalize(version_stmt);
            }
            if (schema_version != MEDIA_CACHE_SCHEMA_VERSION) {
                sqlite3_exec(cache_db, "DROP TABLE IF EXISTS media", NULL, NULL, NULL);
                sqlite3_exec(cache_db, "PRAGMA user_version=" G_STRINGIFY(MEDIA_CACHE_SCHEMA_VERSION), NULL, NULL, NULL);
            }
            if (sqlite3_exec(cache_db,
                             "CREATE TABLE IF NOT EXISTS media ("
                             "path TEXT PRIMARY KEY, mtime INTEGER, size INTEGER, duration_ms INTEGER,"
                             "width INTEGER, height INTEGER, bit_rate INTEGER, video_codec TEXT, frame_rate REAL)",
                             NULL,
                             NULL,
                             NULL)
                == SQLITE_OK) {
                sqlite3_prepare_v2(cache_db,
                                   "INSERT OR REPLACE INTO media VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
                                   -1,
                                   &insert_stmt,
                                   NULL);
                cache_load_from_db();
            }
            else {
                g_warning("[media_info] failed to create cache table: %s", sqlite3_errmsg(cache_db));
            }
        }
        else {
            g_warning("[media_info] failed to open cache database %s", db_path);
            g_clear_pointer(&cache_db, sqlite3_close);
        }
    }
}

static void
cache_ensure_init(void) {
    if (g_once_init_enter(&cache_initialized)) {
        g_rw_lock_writer_lock(&cache_lock);
        cache_init_body();
        g_rw_lock_writer_unlock(&cache_lock);
        g_once_init_leave(&cache_initialized, 1);
    }
}

bool
fsearch_media_info_lookup(const char *path, int64_t mtime, int64_t size, FsearchMediaInfo *out) {
    g_return_val_if_fail(path, false);
    g_return_val_if_fail(out, false);

    // Searches look up many files from several threads at once, so readers don't block each other
    cache_ensure_init();
    g_rw_lock_reader_lock(&cache_lock);
    const CacheEntry *entry = g_hash_table_lookup(cache, path);
    const bool found = entry && entry->mtime == mtime && entry->size == size;
    if (found) {
        *out = entry->info;
    }
    g_rw_lock_reader_unlock(&cache_lock);
    return found;
}

static void
cache_store(const char *path, int64_t mtime, int64_t size, const FsearchMediaInfo *info) {
    CacheEntry *entry = g_new0(CacheEntry, 1);
    entry->mtime = mtime;
    entry->size = size;
    entry->info = *info;

    cache_ensure_init();
    g_rw_lock_writer_lock(&cache_lock);
    g_hash_table_replace(cache, g_strdup(path), entry);
    if (insert_stmt) {
        sqlite3_reset(insert_stmt);
        sqlite3_bind_text(insert_stmt, 1, path, -1, SQLITE_STATIC);
        sqlite3_bind_int64(insert_stmt, 2, mtime);
        sqlite3_bind_int64(insert_stmt, 3, size);
        sqlite3_bind_int64(insert_stmt, 4, info->duration_ms);
        sqlite3_bind_int(insert_stmt, 5, info->width);
        sqlite3_bind_int(insert_stmt, 6, info->height);
        sqlite3_bind_int64(insert_stmt, 7, info->bit_rate);
        sqlite3_bind_text(insert_stmt, 8, info->video_codec, -1, SQLITE_STATIC);
        sqlite3_bind_double(insert_stmt, 9, info->frame_rate);
        sqlite3_step(insert_stmt);
        sqlite3_reset(insert_stmt);
    }
    g_rw_lock_writer_unlock(&cache_lock);
}

typedef struct {
    gint64 deadline_us;
    GCancellable *cancellable;
} InterruptContext;

static int
interrupt_cb(void *opaque) {
    InterruptContext *ctx = opaque;
    if (g_get_monotonic_time() > ctx->deadline_us) {
        return 1;
    }
    if (ctx->cancellable && g_cancellable_is_cancelled(ctx->cancellable)) {
        return 1;
    }
    return 0;
}

static const AVStream *
find_video_stream(const AVFormatContext *fmt) {
    for (unsigned int i = 0; i < fmt->nb_streams; ++i) {
        const AVStream *stream = fmt->streams[i];
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && !(stream->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
            return stream;
        }
    }
    return NULL;
}

// Returns false only if the probe was cancelled. A file which can't be read is a valid result
// (duration_ms == -1), so it's cached and not tried again until the file changes.
static bool
probe_file(const char *path, int64_t file_size, FsearchMediaInfo *info, GCancellable *cancellable) {
    memset(info, 0, sizeof(*info));
    info->duration_ms = -1;

    InterruptContext ictx = {.deadline_us = g_get_monotonic_time() + PROBE_TIMEOUT_US, .cancellable = cancellable};

    AVFormatContext *fmt = avformat_alloc_context();
    if (!fmt) {
        return true;
    }
    fmt->interrupt_callback.callback = interrupt_cb;
    fmt->interrupt_callback.opaque = &ictx;

    if (avformat_open_input(&fmt, path, NULL, NULL) < 0) {
        // fmt is freed by avformat_open_input on failure
        return !(cancellable && g_cancellable_is_cancelled(cancellable));
    }

    const AVStream *video = find_video_stream(fmt);
    if (fmt->duration == AV_NOPTS_VALUE || !video || video->codecpar->width <= 0) {
        // Headers alone weren't enough (some containers), analyze a bit of the stream
        avformat_find_stream_info(fmt, NULL);
        video = find_video_stream(fmt);
    }

    if (cancellable && g_cancellable_is_cancelled(cancellable)) {
        avformat_close_input(&fmt);
        return false;
    }

    if (fmt->duration != AV_NOPTS_VALUE && fmt->duration >= 0) {
        info->duration_ms = fmt->duration / (AV_TIME_BASE / 1000);
    }
    info->bit_rate = fmt->bit_rate > 0 ? fmt->bit_rate : 0;
    if (info->bit_rate == 0 && info->duration_ms > 0 && file_size > 0) {
        // Some containers (e.g. Matroska) don't store the bit rate: use the average over the whole file
        info->bit_rate = (int64_t)((double)file_size * 8.0 * 1000.0 / (double)info->duration_ms);
    }
    if (video) {
        info->width = video->codecpar->width;
        info->height = video->codecpar->height;
        g_strlcpy(info->video_codec, avcodec_get_name(video->codecpar->codec_id), sizeof(info->video_codec));

        AVRational rate = video->avg_frame_rate;
        if (rate.num <= 0 || rate.den <= 0) {
            rate = video->r_frame_rate;
        }
        if (rate.num > 0 && rate.den > 0) {
            info->frame_rate = av_q2d(rate);
        }
    }

    avformat_close_input(&fmt);
    return true;
}

typedef struct {
    const FsearchMediaJob *jobs;
    uint32_t num_jobs;
    gint next;
    GCancellable *cancellable;
} ProbeBatch;

static gpointer
probe_batch_thread(gpointer data) {
    ProbeBatch *batch = data;
    while (!(batch->cancellable && g_cancellable_is_cancelled(batch->cancellable))) {
        const gint idx = g_atomic_int_add(&batch->next, 1);
        if (idx < 0 || (uint32_t)idx >= batch->num_jobs) {
            break;
        }
        const FsearchMediaJob *job = &batch->jobs[idx];
        FsearchMediaInfo info;
        if (probe_file(job->path, job->size, &info, batch->cancellable)) {
            cache_store(job->path, job->mtime, job->size, &info);
        }
    }
    return NULL;
}

void
fsearch_media_info_probe_jobs(const FsearchMediaJob *jobs, uint32_t num_jobs, GCancellable *cancellable) {
    g_return_if_fail(jobs || num_jobs == 0);
    if (num_jobs == 0) {
        return;
    }

    ProbeBatch batch = {.jobs = jobs, .num_jobs = num_jobs, .next = 0, .cancellable = cancellable};

    const uint32_t num_threads = MIN(num_jobs, MAX_PROBE_THREADS);
    GThread *threads[MAX_PROBE_THREADS] = {NULL};
    for (uint32_t i = 0; i < num_threads; ++i) {
        threads[i] = g_thread_new("fsearch-media-probe", probe_batch_thread, &batch);
    }
    for (uint32_t i = 0; i < num_threads; ++i) {
        g_thread_join(threads[i]);
    }
}

typedef struct {
    char *path;
    int64_t mtime;
    int64_t size;
    FsearchMediaInfoDoneFunc done;
    gpointer user_data;
} AsyncTask;

static GMutex async_mutex;
static GThreadPool *async_pool = NULL;
static GHashTable *async_in_flight = NULL; // char * (owned by the task) set

static gboolean
async_done_idle(gpointer data) {
    AsyncTask *task = data;
    if (task->done) {
        task->done(task->user_data);
    }
    g_free(task->path);
    g_free(task);
    return G_SOURCE_REMOVE;
}

static void
async_worker(gpointer data, gpointer user_data) {
    AsyncTask *task = data;
    FsearchMediaInfo info;
    if (probe_file(task->path, task->size, &info, NULL)) {
        cache_store(task->path, task->mtime, task->size, &info);
    }

    g_mutex_lock(&async_mutex);
    g_hash_table_remove(async_in_flight, task->path);
    g_mutex_unlock(&async_mutex);

    g_idle_add(async_done_idle, task);
}

bool
fsearch_media_info_probe_async(const char *path,
                               int64_t mtime,
                               int64_t size,
                               FsearchMediaInfoDoneFunc done,
                               gpointer user_data) {
    g_return_val_if_fail(path, false);

    g_mutex_lock(&async_mutex);
    if (!async_pool) {
        async_pool = g_thread_pool_new(async_worker, NULL, MAX_ASYNC_THREADS, FALSE, NULL);
        async_in_flight = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    }
    if (g_hash_table_contains(async_in_flight, path)) {
        g_mutex_unlock(&async_mutex);
        return false;
    }
    g_hash_table_add(async_in_flight, g_strdup(path));
    g_mutex_unlock(&async_mutex);

    AsyncTask *task = g_new0(AsyncTask, 1);
    task->path = g_strdup(path);
    task->mtime = mtime;
    task->size = size;
    task->done = done;
    task->user_data = user_data;
    g_thread_pool_push(async_pool, task, NULL);
    return true;
}

char *
fsearch_media_info_format_duration(int64_t duration_ms) {
    if (duration_ms < 0) {
        return g_strdup("");
    }
    const int64_t total_seconds = (duration_ms + 500) / 1000;
    const int64_t hours = total_seconds / 3600;
    const int64_t minutes = (total_seconds % 3600) / 60;
    const int64_t seconds = total_seconds % 60;
    if (hours > 0) {
        return g_strdup_printf("%" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT, hours, minutes, seconds);
    }
    return g_strdup_printf("%" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT, minutes, seconds);
}

char *
fsearch_media_info_format_resolution(int32_t width, int32_t height) {
    if (width <= 0 || height <= 0) {
        return g_strdup("");
    }
    return g_strdup_printf("%dx%d", width, height);
}

char *
fsearch_media_info_format_bit_rate(int64_t bit_rate) {
    if (bit_rate <= 0) {
        return g_strdup("");
    }
    if (bit_rate >= 1000 * 1000) {
        return g_strdup_printf("%.1f Mb/s", (double)bit_rate / (1000.0 * 1000.0));
    }
    return g_strdup_printf("%" G_GINT64_FORMAT " kb/s", (bit_rate + 500) / 1000);
}

char *
fsearch_media_info_format_frame_rate(double frame_rate) {
    if (frame_rate <= 0.0) {
        return g_strdup("");
    }
    char buf[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_formatd(buf, sizeof(buf), "%.2f", frame_rate);
    // 30.00 -> 30, 29.97 stays, 25.50 -> 25.5
    if (strchr(buf, '.')) {
        char *end = buf + strlen(buf) - 1;
        while (*end == '0') {
            *end-- = '\0';
        }
        if (*end == '.') {
            *end = '\0';
        }
    }
    return g_strdup(buf);
}

int32_t
fsearch_media_info_resolution_class(int32_t width, int32_t height) {
    if (width <= 0 || height <= 0) {
        return 0;
    }
    const int32_t long_side = MAX(width, height);
    const int32_t short_side = MIN(width, height);

    // Classify by both sides and take the higher class. A film in 2.39:1 has a short side of ~800 but is
    // still 1080p (long side 1920); a 4:3 HD video (1440x1080) has a short side of 1080.
    int32_t by_long = 480;
    if (long_side >= 7000) {
        by_long = 4320;
    }
    else if (long_side >= 3600) {
        by_long = 2160;
    }
    else if (long_side >= 2400) {
        by_long = 1440;
    }
    else if (long_side >= 1800) {
        by_long = 1080;
    }
    else if (long_side >= 1200) {
        by_long = 720;
    }

    int32_t by_short = 480;
    if (short_side >= 4000) {
        by_short = 4320;
    }
    else if (short_side >= 2000) {
        by_short = 2160;
    }
    else if (short_side >= 1300) {
        by_short = 1440;
    }
    else if (short_side >= 1000) {
        by_short = 1080;
    }
    else if (short_side >= 680) {
        by_short = 720;
    }

    return MAX(by_long, by_short);
}

char *
fsearch_media_info_normalize_codec_name(const char *name) {
    g_return_val_if_fail(name, NULL);
    char *lower = g_ascii_strdown(name, -1);
    g_strstrip(lower);

    static const struct {
        const char *alias;
        const char *codec;
    } aliases[] = {
        {"avc", "h264"},       {"avc1", "h264"},      {"x264", "h264"},       {"h.264", "h264"},
        {"h265", "hevc"},      {"h.265", "hevc"},     {"x265", "hevc"},       {"xvid", "mpeg4"},
        {"divx", "mpeg4"},     {"mpeg-4", "mpeg4"},   {"mpeg2", "mpeg2video"}, {"mpeg-2", "mpeg2video"},
        {"mpeg1", "mpeg1video"}, {"wmv", "wmv3"},     {"vc-1", "vc1"},
    };
    for (guint i = 0; i < G_N_ELEMENTS(aliases); ++i) {
        if (!strcmp(lower, aliases[i].alias)) {
            g_free(lower);
            return g_strdup(aliases[i].codec);
        }
    }
    return lower;
}

// ---- Parsing of values typed into search queries and the filter panel ----------------------------

// Durations: "90" (seconds), "90s", "5m", "1.5h", "1h30m", "5:30", "1:02:03".
// `start` is the beginning of the given time and `end` the end of its precision (e.g. "5m" covers 5:00 to
// 5:59), so that "duration:5m" finds all videos of about five minutes.
bool
fsearch_media_info_parse_duration(const char *str, int64_t *start_ms, int64_t *end_ms) {
    if (!str || !*str) {
        return false;
    }
    double total_ms = 0;
    double precision_ms = 1000;

    if (strchr(str, ':')) {
        g_auto(GStrv) parts = g_strsplit(str, ":", -1);
        const guint n = g_strv_length(parts);
        if (n < 2 || n > 3) {
            return false;
        }
        double seconds = 0;
        for (guint i = 0; i < n; ++i) {
            char *endp = NULL;
            const gint64 v = g_ascii_strtoll(parts[i], &endp, 10);
            if (endp == parts[i] || *endp != '\0' || v < 0) {
                return false;
            }
            seconds = seconds * 60 + (double)v;
        }
        total_ms = seconds * 1000;
    }
    else {
        const char *p = str;
        bool any = false;
        double smallest_unit_ms = 0;
        bool has_fraction = false;
        while (*p) {
            char *endp = NULL;
            const double v = g_ascii_strtod(p, &endp);
            if (endp == p || v < 0) {
                return false;
            }
            if (memchr(p, '.', endp - p)) {
                has_fraction = true;
            }
            p = endp;
            double unit_ms = 1000;
            if (g_ascii_strncasecmp(p, "h", 1) == 0) {
                unit_ms = 3600 * 1000.0;
                p += 1;
            }
            else if (g_ascii_strncasecmp(p, "min", 3) == 0) {
                unit_ms = 60 * 1000.0;
                p += 3;
            }
            else if (g_ascii_strncasecmp(p, "m", 1) == 0) {
                unit_ms = 60 * 1000.0;
                p += 1;
            }
            else if (g_ascii_strncasecmp(p, "sec", 3) == 0) {
                p += 3;
            }
            else if (g_ascii_strncasecmp(p, "s", 1) == 0) {
                p += 1;
            }
            else if (*p != '\0') {
                return false;
            }
            total_ms += v * unit_ms;
            if (!any || unit_ms < smallest_unit_ms) {
                smallest_unit_ms = unit_ms;
            }
            any = true;
        }
        if (!any) {
            return false;
        }
        precision_ms = has_fraction ? 1000 : smallest_unit_ms;
    }

    if (start_ms) {
        *start_ms = (int64_t)(total_ms + 0.5);
    }
    if (end_ms) {
        *end_ms = (int64_t)(total_ms + precision_ms + 0.5);
    }
    return true;
}

// Resolution classes: sd, hd (720p), fhd (1080p), qhd (1440p), uhd/4k (2160p), 8k, or numbers like 1080 / 1080p
bool
fsearch_media_info_parse_resolution_class(const char *str, int64_t *num_out, int64_t *num_2_out) {
    g_autofree char *lower = g_ascii_strdown(str, -1);
    int64_t value = 0;
    if (!strcmp(lower, "sd")) {
        value = 480;
    }
    else if (!strcmp(lower, "hd")) {
        value = 720;
    }
    else if (!strcmp(lower, "fhd")) {
        value = 1080;
    }
    else if (!strcmp(lower, "qhd") || !strcmp(lower, "2k")) {
        value = 1440;
    }
    else if (!strcmp(lower, "uhd") || !strcmp(lower, "4k")) {
        value = 2160;
    }
    else if (!strcmp(lower, "8k")) {
        value = 4320;
    }
    else {
        char *endp = NULL;
        const gint64 v = g_ascii_strtoll(lower, &endp, 10);
        if (endp == lower || (*endp != '\0' && strcmp(endp, "p") != 0) || v <= 0) {
            return false;
        }
        if (v <= 576) {
            value = 480;
        }
        else if (v == 720) {
            value = 720;
        }
        else if (v == 1080) {
            value = 1080;
        }
        else if (v == 1440) {
            value = 1440;
        }
        else if (v == 2160) {
            value = 2160;
        }
        else if (v == 4320) {
            value = 4320;
        }
        else {
            return false;
        }
    }
    if (num_out) {
        *num_out = value;
    }
    if (num_2_out) {
        *num_2_out = value;
    }
    return true;
}

// Frame rates like "30", "29.97", "23.976"; the result is in 1/100 fps
bool
fsearch_media_info_parse_frame_rate(const char *str, int64_t *num_out, int64_t *num_2_out) {
    char *endp = NULL;
    const double fps = g_ascii_strtod(str, &endp);
    if (endp == str || *endp != '\0' || fps <= 0.0 || fps > 10000.0) {
        return false;
    }
    const int64_t value = (int64_t)(fps * 100.0 + 0.5);
    if (num_out) {
        *num_out = value;
    }
    if (num_2_out) {
        *num_2_out = value;
    }
    return true;
}

// Bit rates like "800" (kb/s), "800k", "5m", "5mbps", "5000kbps"; the result is in kb/s
bool
fsearch_media_info_parse_bit_rate(const char *str, int64_t *num_out, int64_t *num_2_out) {
    char *endp = NULL;
    const double v = g_ascii_strtod(str, &endp);
    if (endp == str || v < 0) {
        return false;
    }
    g_autofree char *suffix = g_ascii_strdown(endp, -1);
    double factor = 1;
    if (suffix[0] == '\0' || !strcmp(suffix, "k") || !strcmp(suffix, "kb") || !strcmp(suffix, "kbps")
        || !strcmp(suffix, "kb/s")) {
        factor = 1;
    }
    else if (!strcmp(suffix, "m") || !strcmp(suffix, "mb") || !strcmp(suffix, "mbps") || !strcmp(suffix, "mb/s")) {
        factor = 1000;
    }
    else if (!strcmp(suffix, "g") || !strcmp(suffix, "gb") || !strcmp(suffix, "gbps") || !strcmp(suffix, "gb/s")) {
        factor = 1000 * 1000;
    }
    else {
        return false;
    }
    const int64_t value = (int64_t)(v * factor + 0.5);
    if (num_out) {
        *num_out = value;
    }
    if (num_2_out) {
        *num_2_out = value;
    }
    return true;
}

