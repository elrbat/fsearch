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

static GMutex cache_mutex;
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

// Must be called with cache_mutex held
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

// Must be called with cache_mutex held
static void
cache_init_locked(void) {
    if (g_once_init_enter(&cache_initialized)) {
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
        g_once_init_leave(&cache_initialized, 1);
    }
}

bool
fsearch_media_info_lookup(const char *path, int64_t mtime, int64_t size, FsearchMediaInfo *out) {
    g_return_val_if_fail(path, false);
    g_return_val_if_fail(out, false);

    g_mutex_lock(&cache_mutex);
    cache_init_locked();
    const CacheEntry *entry = g_hash_table_lookup(cache, path);
    const bool found = entry && entry->mtime == mtime && entry->size == size;
    if (found) {
        *out = entry->info;
    }
    g_mutex_unlock(&cache_mutex);
    return found;
}

static void
cache_store(const char *path, int64_t mtime, int64_t size, const FsearchMediaInfo *info) {
    CacheEntry *entry = g_new0(CacheEntry, 1);
    entry->mtime = mtime;
    entry->size = size;
    entry->info = *info;

    g_mutex_lock(&cache_mutex);
    cache_init_locked();
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
    g_mutex_unlock(&cache_mutex);
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
