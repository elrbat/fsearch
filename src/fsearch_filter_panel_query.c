#include "fsearch_filter_panel_query.h"

#include "fsearch_media_info.h"
#include "fsearch_size_utils.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

const FsearchFilterPanelFinder fsearch_filter_panel_finders[] = {
    {"empty_files", "Empty files (0 bytes)", "files: size:0", false},
    {"empty_folders", "Empty folders", "empty:", false},
    {"long_paths", "Paths longer than 255 characters", "pathlen:>255", false},
    {"unreadable", "Videos which can't be read (corrupt?)", "media:unreadable", true},
    {"pending", "Videos not analyzed yet", "media:pending", true},
    {"low_res", "Low resolution videos (SD)", "res:sd", true},
};
const unsigned int fsearch_filter_panel_num_finders = G_N_ELEMENTS(fsearch_filter_panel_finders);

typedef bool (*ValueParser)(const char *str, int64_t *start, int64_t *end);

static bool
parse_plain_integer(const char *str, int64_t *start, int64_t *end) {
    char *endp = NULL;
    const gint64 v = g_ascii_strtoll(str, &endp, 10);
    if (endp == str || *endp != '\0' || v < 0) {
        return false;
    }
    if (start) {
        *start = v;
    }
    if (end) {
        *end = v;
    }
    return true;
}

static bool
parse_size(const char *str, int64_t *start, int64_t *end) {
    return fsearch_size_parse(str, start, end);
}

static bool
is_set(const char *s) {
    return s && *s;
}

static char *
trimmed(const char *s) {
    if (!s) {
        return g_strdup("");
    }
    char *copy = g_strdup(s);
    g_strstrip(copy);
    return copy;
}

static void
add_problem(GPtrArray *problems, const char *format, ...) G_GNUC_PRINTF(2, 3);

static void
add_problem(GPtrArray *problems, const char *format, ...) {
    if (!problems) {
        return;
    }
    va_list args;
    va_start(args, format);
    g_ptr_array_add(problems, g_strdup_vprintf(format, args));
    va_end(args);
}

static void
add_condition(GPtrArray *conditions, const char *id, char *label, char *query, bool needs_media_info) {
    FsearchFilterPanelCondition *condition = g_new0(FsearchFilterPanelCondition, 1);
    condition->id = g_strdup(id);
    condition->label = label;
    condition->query = query;
    condition->needs_media_info = needs_media_info;
    g_ptr_array_add(conditions, condition);
}

void
fsearch_filter_panel_condition_free(FsearchFilterPanelCondition *condition) {
    if (!condition) {
        return;
    }
    g_free(condition->id);
    g_free(condition->label);
    g_free(condition->query);
    g_free(condition);
}

// Adds the condition for a "from - to" pair of values (if any of them is valid)
static void
add_range_condition(GPtrArray *conditions,
                    const char *id,
                    const char *field,
                    const char *label,
                    const char *min_in,
                    const char *max_in,
                    ValueParser parse,
                    bool needs_media_info,
                    GPtrArray *problems) {
    g_autofree char *min = trimmed(min_in);
    g_autofree char *max = trimmed(max_in);
    int64_t min_val = 0;
    int64_t max_val = 0;
    bool min_ok = false;
    bool max_ok = false;

    if (is_set(min)) {
        min_ok = parse(min, &min_val, NULL);
        if (!min_ok) {
            add_problem(problems, "%s (min): \"%s\" is not a valid value", label, min);
        }
    }
    if (is_set(max)) {
        max_ok = parse(max, &max_val, NULL);
        if (!max_ok) {
            add_problem(problems, "%s (max): \"%s\" is not a valid value", label, max);
        }
    }
    if (min_ok && max_ok && min_val > max_val) {
        add_problem(problems, "%s: the minimum (%s) is larger than the maximum (%s): this filter is ignored", label, min, max);
        return;
    }
    if (min_ok && max_ok && min_val == max_val) {
        add_condition(conditions, id, g_strdup_printf("%s: %s", label, min), g_strdup_printf("%s:%s", field, min), needs_media_info);
        return;
    }
    if (min_ok && max_ok) {
        add_condition(conditions,
                      id,
                      g_strdup_printf("%s: %s – %s", label, min, max),
                      g_strdup_printf("%s:>=%s %s:<=%s", field, min, field, max),
                      needs_media_info);
    }
    else if (min_ok) {
        add_condition(conditions, id, g_strdup_printf("%s: ≥ %s", label, min), g_strdup_printf("%s:>=%s", field, min), needs_media_info);
    }
    else if (max_ok) {
        add_condition(conditions, id, g_strdup_printf("%s: ≤ %s", label, max), g_strdup_printf("%s:<=%s", field, max), needs_media_info);
    }
}

static char *
normalize_list(const char *input) {
    // "mp4, .mkv  avi" -> "mp4;mkv;avi"
    g_autofree char *copy = g_strdup(input ? input : "");
    g_auto(GStrv) parts = g_strsplit_set(copy, ",; \t", -1);
    GString *result = g_string_new(NULL);
    for (guint i = 0; parts[i]; ++i) {
        const char *p = parts[i];
        while (*p == '.' || *p == '*') {
            p++;
        }
        if (!*p) {
            continue;
        }
        if (result->len) {
            g_string_append_c(result, ';');
        }
        g_string_append(result, p);
    }
    return g_string_free(result, FALSE);
}

static const FsearchFilterPanelFinder *
find_finder(const char *id) {
    if (!is_set(id)) {
        return NULL;
    }
    for (guint i = 0; i < fsearch_filter_panel_num_finders; ++i) {
        if (!strcmp(fsearch_filter_panel_finders[i].id, id)) {
            return &fsearch_filter_panel_finders[i];
        }
    }
    return NULL;
}

bool
fsearch_filter_panel_has_media_conditions(const FsearchFilterPanelValues *v) {
    g_return_val_if_fail(v, false);
    const FsearchFilterPanelFinder *finder = find_finder(v->finder);
    return is_set(v->duration_min) || is_set(v->duration_max) || is_set(v->res_min) || is_set(v->res_max)
           || is_set(v->codec) || is_set(v->fps_min) || is_set(v->fps_max) || is_set(v->bitrate_min)
           || is_set(v->bitrate_max) || (finder && finder->needs_media_info);
}

static const char *
modified_label(const char *value) {
    static const struct {
        const char *value;
        const char *label;
    } labels[] = {
        {"today", "Today"},           {"yesterday", "Yesterday"}, {"pastweek", "Past week"},
        {"pastmonth", "Past month"}, {"thisyear", "This year"},   {"pastyear", "Past year"},
    };
    for (guint i = 0; i < G_N_ELEMENTS(labels); ++i) {
        if (!strcmp(labels[i].value, value)) {
            return labels[i].label;
        }
    }
    return value;
}

GPtrArray *
fsearch_filter_panel_build_conditions(const FsearchFilterPanelValues *v, GPtrArray *problems) {
    g_return_val_if_fail(v, NULL);
    GPtrArray *conditions = g_ptr_array_new_with_free_func((GDestroyNotify)fsearch_filter_panel_condition_free);

    if (v->kind == 1) {
        add_condition(conditions, "type", g_strdup("Files only"), g_strdup("files:"), false);
    }
    else if (v->kind == 2) {
        add_condition(conditions, "type", g_strdup("Folders only"), g_strdup("folders:"), false);
    }

    g_autofree char *extensions = normalize_list(v->extensions);
    if (*extensions) {
        add_condition(conditions, "ext", g_strdup_printf("Extension: %s", extensions), g_strdup_printf("ext:%s", extensions), false);
    }

    add_range_condition(conditions, "size", "size", "Size", v->size_min, v->size_max, parse_size, false, problems);

    g_autofree char *modified = trimmed(v->modified);
    if (*modified) {
        add_condition(conditions, "modified", g_strdup_printf("Modified: %s", modified_label(modified)), g_strdup_printf("dm:%s", modified), false);
    }

    add_range_condition(conditions, "pathlen", "pathlen", "Path length", v->pathlen_min, v->pathlen_max, parse_plain_integer, false, problems);
    add_range_condition(conditions, "duration", "duration", "Duration", v->duration_min, v->duration_max, fsearch_media_info_parse_duration, true, problems);
    add_range_condition(conditions, "res", "res", "Resolution", v->res_min, v->res_max, fsearch_media_info_parse_resolution_class, true, problems);
    add_range_condition(conditions, "fps", "fps", "FPS", v->fps_min, v->fps_max, fsearch_media_info_parse_frame_rate, true, problems);
    add_range_condition(conditions, "bitrate", "bitrate", "Bit rate", v->bitrate_min, v->bitrate_max, fsearch_media_info_parse_bit_rate, true, problems);

    g_autofree char *codec = normalize_list(v->codec);
    if (*codec) {
        add_condition(conditions, "codec", g_strdup_printf("Codec: %s", codec), g_strdup_printf("codec:%s", codec), true);
    }

    const FsearchFilterPanelFinder *finder = find_finder(v->finder);
    if (finder) {
        add_condition(conditions, "finder", g_strdup_printf("Find problems: %s", finder->label), g_strdup(finder->query), finder->needs_media_info);
    }
    return conditions;
}

char *
fsearch_filter_panel_conditions_to_query(GPtrArray *conditions, bool include_media, const char *exclude_id) {
    g_return_val_if_fail(conditions, NULL);
    GString *query = g_string_new(NULL);
    for (guint i = 0; i < conditions->len; ++i) {
        const FsearchFilterPanelCondition *c = g_ptr_array_index(conditions, i);
        if ((!include_media && c->needs_media_info) || (exclude_id && !strcmp(c->id, exclude_id))) {
            continue;
        }
        if (query->len) {
            g_string_append_c(query, ' ');
        }
        g_string_append(query, c->query);
    }
    if (query->len == 0) {
        g_string_free(query, TRUE);
        return NULL;
    }
    return g_string_free(query, FALSE);
}

char *
fsearch_filter_panel_build_query(const FsearchFilterPanelValues *v, bool include_media, GPtrArray *problems) {
    g_return_val_if_fail(v, NULL);
    GPtrArray *conditions = fsearch_filter_panel_build_conditions(v, problems);
    char *query = fsearch_filter_panel_conditions_to_query(conditions, include_media, NULL);
    g_ptr_array_unref(conditions);
    return query;
}
