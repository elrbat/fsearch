#pragma once

#include <glib.h>
#include <stdbool.h>

G_BEGIN_DECLS

// The values of the controls of the filter panel, as typed/selected by the user. Empty strings (or NULL)
// mean "don't filter by this". Kept free of any GTK code so that the query generation can be tested.
typedef struct {
    int kind;                 // 0: files and folders, 1: files only, 2: folders only
    const char *extensions;   // "mp4, mkv" or "mp4;mkv"
    const char *size_min;     // "100mb"
    const char *size_max;
    const char *modified;     // value for dm:, e.g. "today", "pastweek", "pastmonth", "pastyear"
    const char *duration_min; // "30m", "1h30m", "90s"
    const char *duration_max;
    const char *res_min;      // "sd", "hd", "fhd", "qhd", "4k", "8k"
    const char *res_max;
    const char *codec;        // "h264", "hevc;av1"
    const char *fps_min;
    const char *fps_max;
    const char *bitrate_min;  // kb/s, "2000" or "2m"
    const char *bitrate_max;
    const char *pathlen_min;
    const char *pathlen_max;
    const char *finder;       // id of a ready-made search for problems, see fsearch_filter_panel_finders
} FsearchFilterPanelValues;

typedef struct {
    const char *id;
    const char *label;
    const char *query;
    bool needs_media_info;
} FsearchFilterPanelFinder;

extern const FsearchFilterPanelFinder fsearch_filter_panel_finders[];
extern const unsigned int fsearch_filter_panel_num_finders;

// One condition of the filter panel, e.g. the size range or the extensions. Each of them can be removed
// on its own, which is how the panel suggests ways to get results from a search without any.
typedef struct {
    char *id;                // "type", "ext", "size", "modified", "pathlen", "duration", "res", "fps", "bitrate", "codec", "finder"
    char *label;             // for people: "Size: 10mb – 2gb"
    char *query;             // search syntax, e.g. "size:>=10mb size:<=2gb"
    bool needs_media_info;   // depends on information read from inside video files
} FsearchFilterPanelCondition;

void
fsearch_filter_panel_condition_free(FsearchFilterPanelCondition *condition);

// All valid conditions in the order they appear in the query, as FsearchFilterPanelCondition. Free the array
// with g_ptr_array_unref(). Messages about invalid values are added to `problems` (optional).
GPtrArray *
fsearch_filter_panel_build_conditions(const FsearchFilterPanelValues *values, GPtrArray *problems);

// Joins the conditions to a search query; NULL if there is none. `exclude_id` (optional) leaves one condition out,
// `include_media` false leaves out the ones which depend on the contents of video files.
char *
fsearch_filter_panel_conditions_to_query(GPtrArray *conditions, bool include_media, const char *exclude_id);

// Builds the search query for the values, e.g. "files: ext:mp4;mkv res:>=fhd duration:>=30m".
// Returns NULL if nothing is set. With `include_media` false all conditions which depend on information
// read from inside video files are left out (used to find the videos which still need to be analyzed).
// If `problems` is given, a message is added for every invalid value or contradicting range (e.g. a
// minimum which is larger than the maximum). Values with problems are not used in the query.
char *
fsearch_filter_panel_build_query(const FsearchFilterPanelValues *values, bool include_media, GPtrArray *problems);

// True if the values contain conditions which need information from inside video files
bool
fsearch_filter_panel_has_media_conditions(const FsearchFilterPanelValues *values);

G_END_DECLS
