#pragma once

#include <gio/gio.h>
#include <glib.h>
#include <stdbool.h>
#include <stdint.h>

G_BEGIN_DECLS

// Media properties of a (video) file. They live inside the file, so they can't come from the
// file system index. Instead they're read with libavformat on demand and cached in memory and in
// an SQLite database (~/.local/share/fsearch/media_info.db), keyed by path, mtime and size.
typedef struct {
    int64_t duration_ms; // -1 if unknown (not a media file, unreadable, ...)
    int32_t width;
    int32_t height;
    int64_t bit_rate;
    char video_codec[24];
} FsearchMediaInfo;

typedef struct {
    char *path; // full path, owned by the caller
    int64_t mtime;
    int64_t size;
} FsearchMediaJob;

// Cheap check (by file extension, case insensitive, without the dot) if a file may be a video
bool
fsearch_media_info_is_video_extension(const char *extension);

// Returns true and fills `out` if there's a cached result for exactly this version of the file
// (same mtime and size). Never touches the file itself.
bool
fsearch_media_info_lookup(const char *path, int64_t mtime, int64_t size, FsearchMediaInfo *out);

// Reads the media info of all files which aren't cached yet, using several threads.
// Blocks until done or cancelled. Results of finished files are kept when cancelled.
void
fsearch_media_info_probe_jobs(const FsearchMediaJob *jobs, uint32_t num_jobs, GCancellable *cancellable);

typedef void (*FsearchMediaInfoDoneFunc)(gpointer user_data);

// Reads the media info of one file in a background thread. `done` is called in the main loop
// once the result is in the cache. Returns false (and never calls `done`) if the file is already
// being read by an earlier request.
bool
fsearch_media_info_probe_async(const char *path,
                               int64_t mtime,
                               int64_t size,
                               FsearchMediaInfoDoneFunc done,
                               gpointer user_data);

// "H:MM:SS" or "M:SS"; empty string if the duration is unknown. Free with g_free.
char *
fsearch_media_info_format_duration(int64_t duration_ms);

G_END_DECLS
