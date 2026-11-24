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
    int64_t bit_rate; // bits per second, 0 if unknown
    double frame_rate; // frames per second, 0 if unknown
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

// Resolution class of a video as used by the "res:" search function and the filter panel:
// 480 (SD and below), 720, 1080, 1440, 2160 (4K) or 4320 (8K). Both dimensions are taken into account,
// so e.g. 1920x800 (widescreen film), 1440x1080 and portrait 1080x1920 videos are all "1080".
int32_t
fsearch_media_info_resolution_class(int32_t width, int32_t height);

// Maps common codec names to the name libavcodec uses (h265 -> hevc, x264 -> h264, xvid -> mpeg4, ...)
// and lower-cases it. Free with g_free.
char *
fsearch_media_info_normalize_codec_name(const char *name);

// Parsers for the values of the search functions duration:, res:, fps: and bitrate:. They all fill the
// start (and end) of the value in the unit used by the matchers: milliseconds for durations, the resolution
// class (see above), 1/100 fps and kb/s. Return false if the text isn't a valid value.
// Durations: "90" (seconds), "90s", "5m", "1.5h", "1h30m", "5:30", "1:02:03". `end` is the end of the precision
// of the given time (e.g. "5m" covers 5:00 to 5:59).
bool
fsearch_media_info_parse_duration(const char *str, int64_t *start_ms, int64_t *end_ms);

// Resolution classes: sd, hd (720p), fhd (1080p), qhd/2k (1440p), uhd/4k (2160p), 8k, or numbers like 1080 / 1080p
bool
fsearch_media_info_parse_resolution_class(const char *str, int64_t *num_out, int64_t *num_2_out);

// Frame rates like "30", "29.97", "23.976"
bool
fsearch_media_info_parse_frame_rate(const char *str, int64_t *num_out, int64_t *num_2_out);

// Bit rates like "800" (kb/s), "800k", "5m", "5mbps"
bool
fsearch_media_info_parse_bit_rate(const char *str, int64_t *num_out, int64_t *num_2_out);

// "H:MM:SS" or "M:SS"; empty string if the duration is unknown. Free with g_free.
char *
fsearch_media_info_format_duration(int64_t duration_ms);

// "1920x1080"; empty string if unknown. Free with g_free.
char *
fsearch_media_info_format_resolution(int32_t width, int32_t height);

// "4.5 Mb/s" or "800 kb/s"; empty string if unknown. Free with g_free.
char *
fsearch_media_info_format_bit_rate(int64_t bit_rate);

// "30", "29.97", "23.98"; empty string if unknown. Free with g_free.
char *
fsearch_media_info_format_frame_rate(double frame_rate);

G_END_DECLS
