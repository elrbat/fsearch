#include "fsearch_query_matchers.h"
#include "fsearch_database_entry.h"
#include "fsearch_media_info.h"
#include "fsearch_query_node.h"
#include <math.h>
#include <string.h>

uint32_t
fsearch_query_matcher_false(FsearchQueryNode *node, FsearchQueryMatchData *match_data) {
    return 0;
}

uint32_t
fsearch_query_matcher_true(FsearchQueryNode *node, FsearchQueryMatchData *match_data) {
    return 1;
}

uint32_t
fsearch_query_matcher_extension(FsearchQueryNode *node, FsearchQueryMatchData *match_data) {
    if (!node->search_term_list) {
        return 0;
    }
    FsearchDatabaseEntry *entry = fsearch_query_match_data_get_entry(match_data);
    const char *ext = db_entry_get_extension(entry);
    if (!ext) {
        return 0;
    }
    for (uint32_t i = 0; i < node->search_term_list->len; i++) {
        int32_t res = 0;
        if (node->flags & QUERY_FLAG_MATCH_CASE) {
            res = strcmp(ext, g_ptr_array_index(node->search_term_list, i));
        }
        else {
            res = strcasecmp(ext, g_ptr_array_index(node->search_term_list, i));
        }
        if (res == 0) {
            return 1;
        }
        else if (res < 0) {
            return 0;
        }
    }
    return 0;
}

static inline uint32_t
cmp_num(int64_t num, FsearchQueryNode *node) {
    switch (node->comparison_type) {
    case FSEARCH_QUERY_NODE_COMPARISON_EQUAL:
        return num == node->num_start;
    case FSEARCH_QUERY_NODE_COMPARISON_GREATER:
        return num > node->num_start;
    case FSEARCH_QUERY_NODE_COMPARISON_SMALLER:
        return num < node->num_start;
    case FSEARCH_QUERY_NODE_COMPARISON_GREATER_EQ:
        return num >= node->num_start;
    case FSEARCH_QUERY_NODE_COMPARISON_SMALLER_EQ:
        return num <= node->num_start;
    case FSEARCH_QUERY_NODE_COMPARISON_RANGE:
        return node->num_start <= num && num < node->num_end;
    default:
        return 0;
    }
}

uint32_t
fsearch_query_matcher_date_modified(FsearchQueryNode *node, FsearchQueryMatchData *match_data) {
    FsearchDatabaseEntry *entry = fsearch_query_match_data_get_entry(match_data);
    if (entry) {
        const time_t time = db_entry_get_mtime(entry);
        return cmp_num(time, node);
    }
    return 0;
}

uint32_t
fsearch_query_matcher_depth(FsearchQueryNode *node, FsearchQueryMatchData *match_data) {
    FsearchDatabaseEntry *entry = fsearch_query_match_data_get_entry(match_data);
    if (entry) {
        const int64_t depth = db_entry_get_depth(entry);
        return cmp_num(depth, node);
    }
    return 0;
}

uint32_t
fsearch_query_matcher_childcount(FsearchQueryNode *node, FsearchQueryMatchData *match_data) {
    FsearchDatabaseEntry *entry = fsearch_query_match_data_get_entry(match_data);
    if (entry && db_entry_is_folder(entry)) {
        const int64_t num_children = db_entry_folder_get_num_children(entry);
        return cmp_num(num_children, node);
    }
    return 0;
}

uint32_t
fsearch_query_matcher_childfilecount(FsearchQueryNode *node, FsearchQueryMatchData *match_data) {
    FsearchDatabaseEntry *entry = fsearch_query_match_data_get_entry(match_data);
    if (entry && db_entry_is_folder(entry)) {
        const int64_t num_files = db_entry_folder_get_num_files(entry);
        return cmp_num(num_files, node);
    }
    return 0;
}

uint32_t
fsearch_query_matcher_childfoldercount(FsearchQueryNode *node, FsearchQueryMatchData *match_data) {
    FsearchDatabaseEntry *entry = fsearch_query_match_data_get_entry(match_data);
    if (entry && db_entry_is_folder(entry)) {
        const int64_t num_folders = db_entry_folder_get_num_folders(entry);
        return cmp_num(num_folders, node);
    }
    return 0;
}

uint32_t
fsearch_query_matcher_size(FsearchQueryNode *node, FsearchQueryMatchData *match_data) {
    FsearchDatabaseEntry *entry = fsearch_query_match_data_get_entry(match_data);
    if (entry) {
        const int64_t size = db_entry_get_size(entry);
        return cmp_num(size, node);
    }
    return 0;
}

// Looks up the media info of a video file. Returns 0 if the entry isn't a video file at all.
static FsearchQueryMediaStatus
get_media_info(FsearchQueryMatchData *match_data, FsearchMediaInfo *info) {
    FsearchDatabaseEntry *entry = fsearch_query_match_data_get_entry(match_data);
    if (!entry || db_entry_is_folder(entry)) {
        return 0;
    }
    if (!fsearch_media_info_is_video_extension(db_entry_get_extension(entry))) {
        return 0;
    }
    const char *path = fsearch_query_match_data_get_path_str(match_data);
    if (!path || !fsearch_media_info_lookup(path, (int64_t)db_entry_get_mtime(entry), (int64_t)db_entry_get_size(entry), info)) {
        return FSEARCH_QUERY_MEDIA_STATUS_PENDING;
    }
    return info->duration_ms < 0 ? FSEARCH_QUERY_MEDIA_STATUS_UNREADABLE : FSEARCH_QUERY_MEDIA_STATUS_OK;
}

uint32_t
fsearch_query_matcher_media_status(FsearchQueryNode *node, FsearchQueryMatchData *match_data) {
    FsearchMediaInfo info;
    const FsearchQueryMediaStatus status = get_media_info(match_data, &info);
    return status != 0 && status == node->num_start;
}

uint32_t
fsearch_query_matcher_duration(FsearchQueryNode *node, FsearchQueryMatchData *match_data) {
    FsearchMediaInfo info;
    if (get_media_info(match_data, &info) != FSEARCH_QUERY_MEDIA_STATUS_OK) {
        return 0;
    }
    return cmp_num(info.duration_ms, node);
}

uint32_t
fsearch_query_matcher_resolution(FsearchQueryNode *node, FsearchQueryMatchData *match_data) {
    FsearchMediaInfo info;
    if (get_media_info(match_data, &info) != FSEARCH_QUERY_MEDIA_STATUS_OK || info.width <= 0) {
        return 0;
    }
    return cmp_num(fsearch_media_info_resolution_class(info.width, info.height), node);
}

uint32_t
fsearch_query_matcher_width(FsearchQueryNode *node, FsearchQueryMatchData *match_data) {
    FsearchMediaInfo info;
    if (get_media_info(match_data, &info) != FSEARCH_QUERY_MEDIA_STATUS_OK || info.width <= 0) {
        return 0;
    }
    return cmp_num(info.width, node);
}

uint32_t
fsearch_query_matcher_height(FsearchQueryNode *node, FsearchQueryMatchData *match_data) {
    FsearchMediaInfo info;
    if (get_media_info(match_data, &info) != FSEARCH_QUERY_MEDIA_STATUS_OK || info.height <= 0) {
        return 0;
    }
    return cmp_num(info.height, node);
}

uint32_t
fsearch_query_matcher_fps(FsearchQueryNode *node, FsearchQueryMatchData *match_data) {
    FsearchMediaInfo info;
    if (get_media_info(match_data, &info) != FSEARCH_QUERY_MEDIA_STATUS_OK || info.frame_rate <= 0.0) {
        return 0;
    }
    const double fps_100 = info.frame_rate * 100.0;
    if (node->comparison_type == FSEARCH_QUERY_NODE_COMPARISON_EQUAL) {
        // "fps:30" should also find 29.97 and "fps:24" 23.976: allow a deviation of 0.5%
        return fabs(fps_100 - (double)node->num_start) <= (double)node->num_start * 0.005;
    }
    return cmp_num((int64_t)(fps_100 + 0.5), node);
}

uint32_t
fsearch_query_matcher_bitrate(FsearchQueryNode *node, FsearchQueryMatchData *match_data) {
    FsearchMediaInfo info;
    if (get_media_info(match_data, &info) != FSEARCH_QUERY_MEDIA_STATUS_OK || info.bit_rate <= 0) {
        return 0;
    }
    return cmp_num((info.bit_rate + 500) / 1000, node);
}

uint32_t
fsearch_query_matcher_codec(FsearchQueryNode *node, FsearchQueryMatchData *match_data) {
    if (!node->search_term_list) {
        return 0;
    }
    FsearchMediaInfo info;
    if (get_media_info(match_data, &info) != FSEARCH_QUERY_MEDIA_STATUS_OK || !info.video_codec[0]) {
        return 0;
    }
    for (uint32_t i = 0; i < node->search_term_list->len; i++) {
        if (!strcmp(info.video_codec, g_ptr_array_index(node->search_term_list, i))) {
            return 1;
        }
    }
    return 0;
}

uint32_t
fsearch_query_matcher_pathlen(FsearchQueryNode *node, FsearchQueryMatchData *match_data) {
    FsearchDatabaseEntry *entry = fsearch_query_match_data_get_entry(match_data);
    if (!entry) {
        return 0;
    }
    return cmp_num(db_entry_get_path_full_length(entry), node);
}

static void
add_path_highlight(FsearchQueryMatchData *match_data, uint32_t start_idx, uint32_t needle_len) {
    // It's possible that the path highlighting spans across both the path and name string
    const char *name = fsearch_query_match_data_get_name_str(match_data);
    const char *path = fsearch_query_match_data_get_path_str(match_data);
    const size_t name_len = strlen(name);
    const size_t path_len = strlen(path);
    const size_t parent_len = path_len - name_len;

    PangoAttribute *pa = pango_attr_weight_new(PANGO_WEIGHT_BOLD);
    pa->start_index = start_idx;
    if (pa->start_index > parent_len) {
        // the matching part is only in the file name
        pa->start_index -= parent_len;
        pa->end_index = pa->start_index + needle_len;
        fsearch_query_match_data_add_highlight(match_data, pa, DATABASE_INDEX_PROPERTY_NAME);
        return;
    }
    else if (pa->start_index + needle_len > parent_len) {
        // the matching part spans across the path and name
        pa->end_index = G_MAXUINT;
        fsearch_query_match_data_add_highlight(match_data, pa, DATABASE_INDEX_PROPERTY_PATH);
        PangoAttribute *pa_name = pango_attr_weight_new(PANGO_WEIGHT_BOLD);
        pa_name->start_index = 0;
        pa_name->end_index = pa->start_index + needle_len - parent_len;
        fsearch_query_match_data_add_highlight(match_data, pa_name, DATABASE_INDEX_PROPERTY_NAME);
        return;
    }
    else {
        // the matching part is only in the path name
        pa->end_index = pa->start_index + needle_len;
        fsearch_query_match_data_add_highlight(match_data, pa, DATABASE_INDEX_PROPERTY_PATH);
        return;
    }
}

uint32_t
fsearch_query_matcher_regex(FsearchQueryNode *node, FsearchQueryMatchData *match_data) {
    const char *haystack = node->haystack_func(match_data);
    const size_t haystack_len = strlen(haystack);
    if (G_UNLIKELY(!node->regex)) {
        return 0;
    }
    const int32_t thread_id = fsearch_query_match_data_get_thread_id(match_data);
    pcre2_match_data *regex_match_data = g_ptr_array_index(node->regex_match_data_for_threads, thread_id);
    if (G_UNLIKELY(!regex_match_data)) {
        return 0;
    }
    int num_matches = 0;
    if (G_LIKELY(node->regex_jit_available)) {
        num_matches =
            pcre2_jit_match(node->regex, (PCRE2_SPTR)haystack, (PCRE2_SIZE)haystack_len, 0, 0, regex_match_data, NULL);
    }
    else {
        num_matches =
            pcre2_match(node->regex, (PCRE2_SPTR)haystack, (PCRE2_SIZE)haystack_len, 0, 0, regex_match_data, NULL);
    }
    return num_matches > 0 ? 1 : 0;
}

uint32_t
fsearch_query_matcher_utf_strcasestr(FsearchQueryNode *node, FsearchQueryMatchData *match_data) {
    FsearchUtfBuilder *haystack_builder = node->haystack_func(match_data);
    FsearchUtfBuilder *needle_builder = node->needle_builder;
    if (G_LIKELY(haystack_builder->string_is_folded_and_normalized)) {
        return u_strFindFirst(haystack_builder->string_normalized_folded,
                              haystack_builder->string_normalized_folded_len,
                              needle_builder->string_normalized_folded,
                              needle_builder->string_normalized_folded_len)
                 ? 1
                 : 0;
    }
    return 0;
}

uint32_t
fsearch_query_matcher_utf_strcasecmp(FsearchQueryNode *node, FsearchQueryMatchData *match_data) {
    FsearchUtfBuilder *haystack_builder = node->haystack_func(match_data);
    FsearchUtfBuilder *needle_builder = node->needle_builder;
    if (G_LIKELY(haystack_builder->string_is_folded_and_normalized)) {
        return !u_strCompare(haystack_builder->string_normalized_folded,
                             haystack_builder->string_normalized_folded_len,
                             needle_builder->string_normalized_folded,
                             needle_builder->string_normalized_folded_len,
                             false)
                 ? 1
                 : 0;
    }
    return 0;
}

uint32_t
fsearch_query_matcher_strstr(FsearchQueryNode *node, FsearchQueryMatchData *match_data) {
    return strstr(node->haystack_func(match_data), node->needle) ? 1 : 0;
}

uint32_t
fsearch_query_matcher_strcasestr(FsearchQueryNode *node, FsearchQueryMatchData *match_data) {
    return strcasestr(node->haystack_func(match_data), node->needle) ? 1 : 0;
}

uint32_t
fsearch_query_matcher_strcmp(FsearchQueryNode *node, FsearchQueryMatchData *match_data) {
    return !strcmp(node->haystack_func(match_data), node->needle) ? 1 : 0;
}

uint32_t
fsearch_query_matcher_strcasecmp(FsearchQueryNode *node, FsearchQueryMatchData *match_data) {
    return !strcasecmp(node->haystack_func(match_data), node->needle) ? 1 : 0;
}

uint32_t
fsearch_query_matcher_highlight_none(FsearchQueryNode *node, FsearchQueryMatchData *match_data) {
    return 1;
}

uint32_t
fsearch_query_matcher_highlight_extension(FsearchQueryNode *node, FsearchQueryMatchData *match_data) {
    if (!node->search_term_list) {
        return 0;
    }
    if (!fsearch_query_matcher_extension(node, match_data)) {
        return 0;
    }
    FsearchDatabaseEntry *entry = fsearch_query_match_data_get_entry(match_data);
    const char *ext = db_entry_get_extension(entry);
    const char *name = fsearch_query_match_data_get_name_str(match_data);
    if (!name) {
        return 0;
    }
    const size_t name_len = strlen(name);
    const size_t ext_len = strlen(ext);

    PangoAttribute *pa_name = pango_attr_weight_new(PANGO_WEIGHT_BOLD);
    pa_name->start_index = name_len - ext_len;
    pa_name->end_index = G_MAXUINT;
    fsearch_query_match_data_add_highlight(match_data, pa_name, DATABASE_INDEX_PROPERTY_NAME);

    PangoAttribute *pa_ext = pango_attr_weight_new(PANGO_WEIGHT_BOLD);
    fsearch_query_match_data_add_highlight(match_data, pa_ext, DATABASE_INDEX_PROPERTY_EXTENSION);
    return 1;
}

uint32_t
fsearch_query_matcher_highlight_size(FsearchQueryNode *node, FsearchQueryMatchData *match_data) {
    if (fsearch_query_matcher_size(node, match_data)) {
        PangoAttribute *pa = pango_attr_weight_new(PANGO_WEIGHT_BOLD);
        fsearch_query_match_data_add_highlight(match_data, pa, DATABASE_INDEX_PROPERTY_SIZE);
        return 1;
    }
    return 0;
}

uint32_t
fsearch_query_matcher_highlight_regex(FsearchQueryNode *node, FsearchQueryMatchData *match_data) {
    const bool search_in_path = node->flags & QUERY_FLAG_SEARCH_IN_PATH;
    const char *haystack = node->haystack_func(match_data);
    const int32_t thread_id = fsearch_query_match_data_get_thread_id(match_data);
    const size_t haystack_len = strlen(haystack);
    pcre2_match_data *regex_match_data = g_ptr_array_index(node->regex_match_data_for_threads, thread_id);
    if (!regex_match_data) {
        return 0;
    }
    const int num_matches =
        pcre2_match(node->regex, (PCRE2_SPTR)haystack, (PCRE2_SIZE)haystack_len, 0, 0, regex_match_data, NULL);
    if (num_matches <= 0) {
        return 0;
    }

    PCRE2_SIZE *ovector = pcre2_get_ovector_pointer(regex_match_data);
    for (int i = 0; i < num_matches; i++) {
        const uint32_t start_idx = ovector[2 * i];
        const uint32_t end_idx = ovector[2 * i + 1];
        if (!search_in_path) {
            PangoAttribute *pa = pango_attr_weight_new(PANGO_WEIGHT_BOLD);
            pa->start_index = start_idx;
            pa->end_index = end_idx;
            fsearch_query_match_data_add_highlight(match_data, pa, DATABASE_INDEX_PROPERTY_NAME);
        }
        else {
            add_path_highlight(match_data, start_idx, end_idx - start_idx);
        }
    }
    return 1;
}

uint32_t
fsearch_query_matcher_highlight_ascii(FsearchQueryNode *node, FsearchQueryMatchData *match_data) {
    const bool search_in_path = node->flags & QUERY_FLAG_SEARCH_IN_PATH;
    const char *haystack = node->haystack_func(match_data);
    if (node->flags & QUERY_FLAG_EXACT_MATCH) {
        if (node->flags & QUERY_FLAG_MATCH_CASE ? !strcmp(haystack, node->needle) : !strcasecmp(haystack, node->needle)) {
            PangoAttribute *pa = pango_attr_weight_new(PANGO_WEIGHT_BOLD);
            fsearch_query_match_data_add_highlight(match_data, pa, DATABASE_INDEX_PROPERTY_NAME);
            if (search_in_path) {
                PangoAttribute *pa_path = pango_attr_weight_new(PANGO_WEIGHT_BOLD);
                fsearch_query_match_data_add_highlight(match_data, pa_path, DATABASE_INDEX_PROPERTY_PATH);
            }
            return 1;
        }
        return 0;
    }
    const char *dest = node->flags & QUERY_FLAG_MATCH_CASE ? strstr(haystack, node->needle)
                                                           : strcasestr(haystack, node->needle);
    if (!dest) {
        return 0;
    }
    if (search_in_path) {
        const size_t needle_len = strlen(node->needle);
        add_path_highlight(match_data, dest - haystack, needle_len);
    }
    else {
        PangoAttribute *pa = pango_attr_weight_new(PANGO_WEIGHT_BOLD);
        pa->start_index = dest - haystack;
        pa->end_index = pa->start_index + strlen(node->needle);
        fsearch_query_match_data_add_highlight(match_data, pa, DATABASE_INDEX_PROPERTY_NAME);
    }
    return 1;
}