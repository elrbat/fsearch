#include "fsearch_filter_panel.h"

#include "fsearch_filter_panel_query.h"

#include <glib/gi18n.h>
#include <stdarg.h>
#include <string.h>

#define PANEL_DATA_KEY "fsearch-filter-panel-data"
#define ENTRY_DELAY_MS 500

typedef struct {
    GtkWidget *revealer;

    GtkWidget *kind;
    GtkWidget *ext;
    GtkWidget *ext_presets;
    GtkWidget *size_min;
    GtkWidget *size_max;
    GtkWidget *modified;
    GtkWidget *duration_min;
    GtkWidget *duration_max;
    GtkWidget *res_min;
    GtkWidget *res_max;
    GtkWidget *codec;
    GtkWidget *fps_min;
    GtkWidget *fps_max;
    GtkWidget *bitrate_min;
    GtkWidget *bitrate_max;
    GtkWidget *pathlen_min;
    GtkWidget *pathlen_max;
    GtkWidget *finder;
    GtkWidget *smart_matching;

    GtkWidget *summary_label;
    GtkWidget *warning_label;
    GtkWidget *media_note_box;
    GtkWidget *analyze_button;
    GtkWidget *analyze_label;
    GtkWidget *suggestion_box;
    GtkWidget *suggestion_label;
    GtkWidget *suggestion_flow;

    FsearchFilterPanelChangedFunc changed;
    FsearchFilterPanelAnalyzeFunc analyze;
    FsearchFilterPanelSuggestionFunc suggestion;
    gpointer user_data;

    guint changed_timeout_id;
    bool resetting;
    bool analyzing;
} PanelData;

static const char *modified_values[] = {"", "today", "yesterday", "pastweek", "pastmonth", "thisyear", "pastyear"};
static const char *modified_labels[] = {N_("Any time"), N_("Today"), N_("Yesterday"), N_("Past week"), N_("Past month"), N_("This year"), N_("Past year")};

static const char *res_values[] = {"", "sd", "hd", "fhd", "qhd", "4k", "8k"};
static const char *res_labels[] = {N_("Any"), N_("SD"), N_("HD 720p"), N_("FHD 1080p"), N_("QHD 1440p"), N_("4K"), N_("8K")};

static const struct {
    const char *label;
    const char *extensions;
} ext_presets[] = {
    {N_("Videos"), "mp4;mkv;avi;mov;wmv;flv;webm;m4v;mpg;mpeg;ts;m2ts"},
    {N_("Audio"), "mp3;flac;wav;aac;ogg;m4a;opus;wma"},
    {N_("Images"), "jpg;jpeg;png;gif;webp;bmp;tiff;svg"},
    {N_("Documents"), "pdf;doc;docx;xls;xlsx;ppt;pptx;txt;odt;ods;epub"},
    {N_("Archives"), "zip;rar;7z;tar;gz;bz2;xz"},
};

static const char *codec_names[] = {"h264", "hevc", "av1", "vp9", "mpeg4", "mpeg2video", "wmv3"};

static void
install_css(void) {
    static bool installed = false;
    if (installed) {
        return;
    }
    installed = true;
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(provider,
                                    ".fsearch-filter-panel { padding: 6px 8px; border-bottom: 1px solid alpha(currentColor, 0.18); }"
                                    ".fsearch-filter-title { font-weight: bold; opacity: 0.75; }"
                                    ".fsearch-filter-summary { opacity: 0.7; font-size: smaller; }"
                                    ".fsearch-filter-warning { color: #c01c28; }"
                                    ".fsearch-filter-note { font-size: smaller; }",
                                    -1,
                                    NULL);
    GdkScreen *screen = gdk_screen_get_default();
    if (screen) {
        gtk_style_context_add_provider_for_screen(screen, GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    g_object_unref(provider);
}

static PanelData *
get_data(GtkWidget *panel) {
    return g_object_get_data(G_OBJECT(panel), PANEL_DATA_KEY);
}

static const char *
entry_text(GtkWidget *entry) {
    return gtk_entry_get_text(GTK_ENTRY(entry));
}

static const char *
combo_value(GtkWidget *combo, const char *const *values, guint n) {
    const gint active = gtk_combo_box_get_active(GTK_COMBO_BOX(combo));
    if (active < 0 || (guint)active >= n) {
        return "";
    }
    return values[active];
}

static void
collect_values(PanelData *d, FsearchFilterPanelValues *v, char **finder_id_out) {
    memset(v, 0, sizeof(*v));
    v->kind = gtk_combo_box_get_active(GTK_COMBO_BOX(d->kind));
    v->extensions = entry_text(d->ext);
    v->size_min = entry_text(d->size_min);
    v->size_max = entry_text(d->size_max);
    v->modified = combo_value(d->modified, modified_values, G_N_ELEMENTS(modified_values));
    v->duration_min = entry_text(d->duration_min);
    v->duration_max = entry_text(d->duration_max);
    v->res_min = combo_value(d->res_min, res_values, G_N_ELEMENTS(res_values));
    v->res_max = combo_value(d->res_max, res_values, G_N_ELEMENTS(res_values));
    v->codec = gtk_entry_get_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(d->codec))));
    v->fps_min = entry_text(d->fps_min);
    v->fps_max = entry_text(d->fps_max);
    v->bitrate_min = entry_text(d->bitrate_min);
    v->bitrate_max = entry_text(d->bitrate_max);
    v->pathlen_min = entry_text(d->pathlen_min);
    v->pathlen_max = entry_text(d->pathlen_max);

    const gint finder = gtk_combo_box_get_active(GTK_COMBO_BOX(d->finder));
    *finder_id_out = NULL;
    if (finder > 0 && (guint)finder <= fsearch_filter_panel_num_finders) {
        *finder_id_out = g_strdup(fsearch_filter_panel_finders[finder - 1].id);
        v->finder = *finder_id_out;
    }
}

static void
update_summary(PanelData *d) {
    FsearchFilterPanelValues v;
    g_autofree char *finder_id = NULL;
    collect_values(d, &v, &finder_id);

    GPtrArray *problems = g_ptr_array_new_with_free_func(g_free);
    g_autofree char *query = fsearch_filter_panel_build_query(&v, true, problems);

    if (query) {
        g_autofree char *text = g_strdup_printf(_("Active filters: %s"), query);
        gtk_label_set_text(GTK_LABEL(d->summary_label), text);
    }
    else {
        gtk_label_set_text(GTK_LABEL(d->summary_label), _("No filters active"));
    }

    if (problems->len > 0) {
        GString *msg = g_string_new(NULL);
        for (guint i = 0; i < problems->len; ++i) {
            if (i) {
                g_string_append_c(msg, '\n');
            }
            g_string_append_printf(msg, "⚠ %s", (char *)g_ptr_array_index(problems, i));
        }
        gtk_label_set_text(GTK_LABEL(d->warning_label), msg->str);
        gtk_widget_set_visible(d->warning_label, TRUE);
        g_string_free(msg, TRUE);
    }
    else {
        gtk_widget_set_visible(d->warning_label, FALSE);
    }
    g_ptr_array_unref(problems);

    // Only conditions which really end up in the query count, not the ones with problems
    g_autofree char *query_without_media = fsearch_filter_panel_build_query(&v, false, NULL);
    const bool media = g_strcmp0(query, query_without_media) != 0;
    gtk_widget_set_visible(d->media_note_box, media);
    gtk_widget_set_sensitive(d->analyze_button, !d->analyzing);
    gtk_label_set_text(GTK_LABEL(d->analyze_label),
                       d->analyzing ? _("Reading the videos… (the first time this can take a while)")
                                    : _("Video filters only match videos that were already analyzed. Videos which weren't read yet are left out."));
}

static gboolean
changed_timeout_cb(gpointer user_data) {
    PanelData *d = user_data;
    d->changed_timeout_id = 0;
    if (d->changed) {
        d->changed(d->user_data);
    }
    return G_SOURCE_REMOVE;
}

static void
schedule_changed(PanelData *d, guint delay_ms) {
    update_summary(d);
    if (d->resetting) {
        return;
    }
    if (d->changed_timeout_id) {
        g_source_remove(d->changed_timeout_id);
    }
    d->changed_timeout_id = g_timeout_add(delay_ms, changed_timeout_cb, d);
}

static void
on_entry_changed(GtkEditable *editable, gpointer user_data) {
    schedule_changed(user_data, ENTRY_DELAY_MS);
}

static void
on_combo_changed(GtkComboBox *combo, gpointer user_data) {
    schedule_changed(user_data, 50);
}

static void
on_check_toggled(GtkToggleButton *button, gpointer user_data) {
    schedule_changed(user_data, 50);
}

static void
on_ext_preset_changed(GtkComboBox *combo, gpointer user_data) {
    PanelData *d = user_data;
    const gint active = gtk_combo_box_get_active(combo);
    if (active > 0 && (guint)active <= G_N_ELEMENTS(ext_presets)) {
        gtk_entry_set_text(GTK_ENTRY(d->ext), ext_presets[active - 1].extensions);
    }
    // back to the title so that the same preset can be chosen again
    g_signal_handlers_block_by_func(combo, on_ext_preset_changed, d);
    gtk_combo_box_set_active(combo, 0);
    g_signal_handlers_unblock_by_func(combo, on_ext_preset_changed, d);
}

static void
on_reset_clicked(GtkButton *button, gpointer user_data) {
    fsearch_filter_panel_widget_reset(GTK_WIDGET(((PanelData *)user_data)->revealer));
}

static void
on_analyze_clicked(GtkButton *button, gpointer user_data) {
    PanelData *d = user_data;
    if (d->analyze && !d->analyzing) {
        d->analyze(d->user_data);
    }
}

static GtkWidget *
new_entry(PanelData *d, const char *placeholder, int width_chars, const char *tooltip) {
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_width_chars(GTK_ENTRY(entry), width_chars);
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), placeholder);
    gtk_entry_set_max_width_chars(GTK_ENTRY(entry), width_chars + 6);
    if (tooltip) {
        gtk_widget_set_tooltip_text(entry, tooltip);
    }
    g_signal_connect(entry, "changed", G_CALLBACK(on_entry_changed), d);
    return entry;
}

static GtkWidget *
new_text_combo(PanelData *d, const char *const *labels, guint n, bool translate) {
    GtkWidget *combo = gtk_combo_box_text_new();
    for (guint i = 0; i < n; ++i) {
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), translate ? _(labels[i]) : labels[i]);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 0);
    g_signal_connect(combo, "changed", G_CALLBACK(on_combo_changed), d);
    return combo;
}

static GtkWidget *
new_group(const char *title, GtkWidget *first, ...) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    if (title) {
        GtkWidget *label = gtk_label_new(title);
        gtk_style_context_add_class(gtk_widget_get_style_context(label), "fsearch-filter-title");
        gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);
    }
    va_list args;
    va_start(args, first);
    for (GtkWidget *w = first; w; w = va_arg(args, GtkWidget *)) {
        gtk_box_pack_start(GTK_BOX(box), w, FALSE, FALSE, 0);
    }
    va_end(args);
    gtk_widget_set_margin_end(box, 14);
    gtk_widget_set_margin_top(box, 2);
    gtk_widget_set_margin_bottom(box, 2);
    return box;
}

static GtkWidget *
dash(void) {
    return gtk_label_new("–");
}

static void
free_panel_data(gpointer data) {
    PanelData *d = data;
    if (d->changed_timeout_id) {
        g_source_remove(d->changed_timeout_id);
    }
    g_free(d);
}

GtkWidget *
fsearch_filter_panel_widget_new(FsearchFilterPanelChangedFunc changed,
                                FsearchFilterPanelAnalyzeFunc analyze,
                                FsearchFilterPanelSuggestionFunc suggestion,
                                gpointer user_data) {
    install_css();

    PanelData *d = g_new0(PanelData, 1);
    d->changed = changed;
    d->analyze = analyze;
    d->suggestion = suggestion;
    d->user_data = user_data;

    d->revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(d->revealer), GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    gtk_revealer_set_reveal_child(GTK_REVEALER(d->revealer), FALSE);
    g_object_set_data_full(G_OBJECT(d->revealer), PANEL_DATA_KEY, d, free_panel_data);

    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_style_context_add_class(gtk_widget_get_style_context(outer), "fsearch-filter-panel");
    gtk_container_add(GTK_CONTAINER(d->revealer), outer);

    GtkWidget *flow = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(flow), GTK_SELECTION_NONE);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(flow), 12);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(flow), 0);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(flow), 0);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(flow), FALSE);
    gtk_box_pack_start(GTK_BOX(outer), flow, FALSE, TRUE, 0);

    // --- general -------------------------------------------------------------------------------
    const char *kinds[] = {N_("Files and folders"), N_("Files only"), N_("Folders only")};
    d->kind = new_text_combo(d, kinds, G_N_ELEMENTS(kinds), true);
    gtk_flow_box_insert(GTK_FLOW_BOX(flow), new_group(_("Type"), d->kind, NULL), -1);

    d->ext = new_entry(d, "mp4;mkv", 14, _("File extensions, separated by ; or ,"));
    d->ext_presets = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(d->ext_presets), _("Presets…"));
    for (guint i = 0; i < G_N_ELEMENTS(ext_presets); ++i) {
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(d->ext_presets), _(ext_presets[i].label));
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(d->ext_presets), 0);
    g_signal_connect(d->ext_presets, "changed", G_CALLBACK(on_ext_preset_changed), d);
    gtk_flow_box_insert(GTK_FLOW_BOX(flow), new_group(_("Extension"), d->ext, d->ext_presets, NULL), -1);

    d->size_min = new_entry(d, "10mb", 7, _("Smallest size, e.g. 500kb, 10mb, 2gb"));
    d->size_max = new_entry(d, "2gb", 7, _("Largest size"));
    gtk_flow_box_insert(GTK_FLOW_BOX(flow), new_group(_("Size"), d->size_min, dash(), d->size_max, NULL), -1);

    d->modified = new_text_combo(d, modified_labels, G_N_ELEMENTS(modified_labels), true);
    gtk_flow_box_insert(GTK_FLOW_BOX(flow), new_group(_("Modified"), d->modified, NULL), -1);

    d->pathlen_min = new_entry(d, "0", 5, _("Shortest full path, in characters"));
    d->pathlen_max = new_entry(d, "255", 5, _("Longest full path, in characters"));
    gtk_flow_box_insert(GTK_FLOW_BOX(flow), new_group(_("Path length"), d->pathlen_min, dash(), d->pathlen_max, NULL), -1);

    // --- video properties -------------------------------------------------------------------------
    d->duration_min = new_entry(d, "30m", 6, _("Shortest duration, e.g. 90s, 5m, 1h30m"));
    d->duration_max = new_entry(d, "2h", 6, _("Longest duration"));
    gtk_flow_box_insert(GTK_FLOW_BOX(flow), new_group(_("Duration"), d->duration_min, dash(), d->duration_max, NULL), -1);

    d->res_min = new_text_combo(d, res_labels, G_N_ELEMENTS(res_labels), true);
    d->res_max = new_text_combo(d, res_labels, G_N_ELEMENTS(res_labels), true);
    gtk_widget_set_tooltip_text(d->res_min, _("At least this resolution"));
    gtk_widget_set_tooltip_text(d->res_max, _("At most this resolution"));
    gtk_flow_box_insert(GTK_FLOW_BOX(flow), new_group(_("Resolution"), d->res_min, dash(), d->res_max, NULL), -1);

    d->codec = gtk_combo_box_text_new_with_entry();
    for (guint i = 0; i < G_N_ELEMENTS(codec_names); ++i) {
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(d->codec), codec_names[i]);
    }
    GtkWidget *codec_entry = gtk_bin_get_child(GTK_BIN(d->codec));
    gtk_entry_set_width_chars(GTK_ENTRY(codec_entry), 10);
    gtk_entry_set_placeholder_text(GTK_ENTRY(codec_entry), "h264;hevc");
    gtk_widget_set_tooltip_text(d->codec, _("Video codec, several can be separated by ;"));
    g_signal_connect(codec_entry, "changed", G_CALLBACK(on_entry_changed), d);
    gtk_flow_box_insert(GTK_FLOW_BOX(flow), new_group(_("Codec"), d->codec, NULL), -1);

    d->fps_min = new_entry(d, "24", 5, _("Lowest frame rate"));
    d->fps_max = new_entry(d, "60", 5, _("Highest frame rate"));
    gtk_flow_box_insert(GTK_FLOW_BOX(flow), new_group(_("FPS"), d->fps_min, dash(), d->fps_max, NULL), -1);

    d->bitrate_min = new_entry(d, "2m", 6, _("Lowest bit rate in kb/s, e.g. 800, 2m (= 2000)"));
    d->bitrate_max = new_entry(d, "20m", 6, _("Highest bit rate"));
    gtk_flow_box_insert(GTK_FLOW_BOX(flow), new_group(_("Bit rate"), d->bitrate_min, dash(), d->bitrate_max, NULL), -1);

    // --- ready made searches and buttons -------------------------------------------------------------
    d->finder = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(d->finder), _("None"));
    for (guint i = 0; i < fsearch_filter_panel_num_finders; ++i) {
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(d->finder), _(fsearch_filter_panel_finders[i].label));
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(d->finder), 0);
    g_signal_connect(d->finder, "changed", G_CALLBACK(on_combo_changed), d);
    gtk_flow_box_insert(GTK_FLOW_BOX(flow), new_group(_("Find problems"), d->finder, NULL), -1);

    d->smart_matching = gtk_check_button_new_with_label(_("Smart matching"));
    gtk_widget_set_tooltip_text(d->smart_matching,
                                _("Words in the search box match with any of the separators . _ - and spaces "
                                  "(\"movie name\" finds Movie.Name and movie_name), and Arabic letters match "
                                  "their variants (أ إ آ = ا, ة = ه, ى = ي), ignoring diacritics and digit styles."));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->smart_matching), TRUE);
    g_signal_connect(d->smart_matching, "toggled", G_CALLBACK(on_check_toggled), d);
    gtk_flow_box_insert(GTK_FLOW_BOX(flow), new_group(NULL, d->smart_matching, NULL), -1);

    GtkWidget *reset = gtk_button_new_with_label(_("Reset filters"));
    g_signal_connect(reset, "clicked", G_CALLBACK(on_reset_clicked), d);
    gtk_flow_box_insert(GTK_FLOW_BOX(flow), new_group(NULL, reset, NULL), -1);

    // --- information rows -------------------------------------------------------------------------
    d->summary_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(d->summary_label), 0.0f);
    gtk_label_set_selectable(GTK_LABEL(d->summary_label), TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(d->summary_label), PANGO_ELLIPSIZE_END);
    gtk_style_context_add_class(gtk_widget_get_style_context(d->summary_label), "fsearch-filter-summary");
    gtk_box_pack_start(GTK_BOX(outer), d->summary_label, FALSE, TRUE, 0);

    d->warning_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(d->warning_label), 0.0f);
    gtk_label_set_line_wrap(GTK_LABEL(d->warning_label), TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(d->warning_label), "fsearch-filter-warning");
    gtk_box_pack_start(GTK_BOX(outer), d->warning_label, FALSE, TRUE, 0);
    gtk_widget_set_no_show_all(d->warning_label, TRUE);

    d->media_note_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    d->analyze_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(d->analyze_label), 0.0f);
    gtk_label_set_line_wrap(GTK_LABEL(d->analyze_label), TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(d->analyze_label), "fsearch-filter-note");
    d->analyze_button = gtk_button_new_with_label(_("Analyze videos"));
    gtk_widget_set_tooltip_text(d->analyze_button,
                                _("Reads duration, resolution, codec, frame rate and bit rate of all videos found "
                                  "by the other filters. Already analyzed videos are skipped."));
    g_signal_connect(d->analyze_button, "clicked", G_CALLBACK(on_analyze_clicked), d);
    gtk_box_pack_start(GTK_BOX(d->media_note_box), d->analyze_label, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(d->media_note_box), d->analyze_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), d->media_note_box, FALSE, TRUE, 0);
    gtk_widget_set_no_show_all(d->media_note_box, TRUE);

    d->suggestion_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    d->suggestion_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(d->suggestion_label), 0.0f);
    gtk_label_set_line_wrap(GTK_LABEL(d->suggestion_label), TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(d->suggestion_label), "fsearch-filter-title");
    d->suggestion_flow = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(d->suggestion_flow), GTK_SELECTION_NONE);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(d->suggestion_flow), 6);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(d->suggestion_flow), FALSE);
    gtk_box_pack_start(GTK_BOX(d->suggestion_box), d->suggestion_label, FALSE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(d->suggestion_box), d->suggestion_flow, FALSE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(outer), d->suggestion_box, FALSE, TRUE, 0);
    gtk_widget_set_no_show_all(d->suggestion_box, TRUE);

    gtk_widget_show_all(d->revealer);
    // The children of widgets with "no show all" aren't shown by show_all, but must be visible
    // themselves when their parent gets shown later
    gtk_widget_show(d->analyze_label);
    gtk_widget_show(d->analyze_button);
    gtk_widget_show(d->suggestion_label);
    gtk_widget_show(d->suggestion_flow);
    gtk_widget_set_visible(d->warning_label, FALSE);
    gtk_widget_set_visible(d->media_note_box, FALSE);
    gtk_widget_set_visible(d->suggestion_box, FALSE);
    update_summary(d);
    return d->revealer;
}

void
fsearch_filter_panel_widget_set_revealed(GtkWidget *panel, gboolean revealed) {
    gtk_revealer_set_reveal_child(GTK_REVEALER(panel), revealed);
}

gboolean
fsearch_filter_panel_widget_get_revealed(GtkWidget *panel) {
    return gtk_revealer_get_reveal_child(GTK_REVEALER(panel));
}

char *
fsearch_filter_panel_widget_get_query(GtkWidget *panel, bool include_media) {
    PanelData *d = get_data(panel);
    FsearchFilterPanelValues v;
    g_autofree char *finder_id = NULL;
    collect_values(d, &v, &finder_id);
    return fsearch_filter_panel_build_query(&v, include_media, NULL);
}

bool
fsearch_filter_panel_widget_has_media_conditions(GtkWidget *panel) {
    PanelData *d = get_data(panel);
    FsearchFilterPanelValues v;
    g_autofree char *finder_id = NULL;
    collect_values(d, &v, &finder_id);
    g_autofree char *with_media = fsearch_filter_panel_build_query(&v, true, NULL);
    g_autofree char *without_media = fsearch_filter_panel_build_query(&v, false, NULL);
    return g_strcmp0(with_media, without_media) != 0;
}

void
fsearch_filter_panel_widget_reset(GtkWidget *panel) {
    PanelData *d = get_data(panel);
    d->resetting = true;
    GtkWidget *entries[] = {d->ext, d->size_min, d->size_max, d->duration_min, d->duration_max, d->fps_min, d->fps_max,
                            d->bitrate_min, d->bitrate_max, d->pathlen_min, d->pathlen_max,
                            gtk_bin_get_child(GTK_BIN(d->codec))};
    for (guint i = 0; i < G_N_ELEMENTS(entries); ++i) {
        gtk_entry_set_text(GTK_ENTRY(entries[i]), "");
    }
    GtkWidget *combos[] = {d->kind, d->modified, d->res_min, d->res_max, d->finder};
    for (guint i = 0; i < G_N_ELEMENTS(combos); ++i) {
        gtk_combo_box_set_active(GTK_COMBO_BOX(combos[i]), 0);
    }
    d->resetting = false;
    schedule_changed(d, 0);
}

void
fsearch_filter_panel_widget_set_analyzing(GtkWidget *panel, bool analyzing) {
    PanelData *d = get_data(panel);
    d->analyzing = analyzing;
    update_summary(d);
}

gboolean
fsearch_filter_panel_widget_get_smart_matching(GtkWidget *panel) {
    PanelData *d = get_data(panel);
    return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->smart_matching));
}

void
fsearch_filter_panel_widget_set_smart_matching(GtkWidget *panel, gboolean enabled) {
    PanelData *d = get_data(panel);
    d->resetting = true;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->smart_matching), enabled);
    d->resetting = false;
}

GPtrArray *
fsearch_filter_panel_widget_get_conditions(GtkWidget *panel) {
    PanelData *d = get_data(panel);
    FsearchFilterPanelValues v;
    g_autofree char *finder_id = NULL;
    collect_values(d, &v, &finder_id);
    return fsearch_filter_panel_build_conditions(&v, NULL);
}

char *
fsearch_filter_panel_widget_get_query_excluding(GtkWidget *panel, bool include_media, const char *exclude_id) {
    g_autoptr(GPtrArray) conditions = fsearch_filter_panel_widget_get_conditions(panel);
    return fsearch_filter_panel_conditions_to_query(conditions, include_media, exclude_id);
}

static void
clear_entries(GtkWidget **entries, guint n) {
    for (guint i = 0; i < n; ++i) {
        gtk_entry_set_text(GTK_ENTRY(entries[i]), "");
    }
}

static void
reset_combos(GtkWidget **combos, guint n) {
    for (guint i = 0; i < n; ++i) {
        gtk_combo_box_set_active(GTK_COMBO_BOX(combos[i]), 0);
    }
}

void
fsearch_filter_panel_widget_remove_condition(GtkWidget *panel, const char *id) {
    PanelData *d = get_data(panel);
    d->resetting = true;
    if (!strcmp(id, "type")) {
        reset_combos((GtkWidget *[]){d->kind}, 1);
    }
    else if (!strcmp(id, "ext")) {
        clear_entries((GtkWidget *[]){d->ext}, 1);
    }
    else if (!strcmp(id, "size")) {
        clear_entries((GtkWidget *[]){d->size_min, d->size_max}, 2);
    }
    else if (!strcmp(id, "modified")) {
        reset_combos((GtkWidget *[]){d->modified}, 1);
    }
    else if (!strcmp(id, "pathlen")) {
        clear_entries((GtkWidget *[]){d->pathlen_min, d->pathlen_max}, 2);
    }
    else if (!strcmp(id, "duration")) {
        clear_entries((GtkWidget *[]){d->duration_min, d->duration_max}, 2);
    }
    else if (!strcmp(id, "res")) {
        reset_combos((GtkWidget *[]){d->res_min, d->res_max}, 2);
    }
    else if (!strcmp(id, "fps")) {
        clear_entries((GtkWidget *[]){d->fps_min, d->fps_max}, 2);
    }
    else if (!strcmp(id, "bitrate")) {
        clear_entries((GtkWidget *[]){d->bitrate_min, d->bitrate_max}, 2);
    }
    else if (!strcmp(id, "codec")) {
        clear_entries((GtkWidget *[]){gtk_bin_get_child(GTK_BIN(d->codec))}, 1);
    }
    else if (!strcmp(id, "finder")) {
        reset_combos((GtkWidget *[]){d->finder}, 1);
    }
    d->resetting = false;
    schedule_changed(d, 0);
}

static void
on_suggestion_clicked(GtkButton *button, gpointer user_data) {
    PanelData *d = user_data;
    const char *id = g_object_get_data(G_OBJECT(button), "condition-id");
    g_autofree char *id_copy = g_strdup(id);
    if (!id_copy) {
        return;
    }
    if (!strcmp(id_copy, "text")) {
        if (d->suggestion) {
            d->suggestion(id_copy, d->user_data);
        }
    }
    else {
        fsearch_filter_panel_widget_remove_condition(d->revealer, id_copy);
    }
}

void
fsearch_filter_panel_widget_clear_suggestions(GtkWidget *panel) {
    PanelData *d = get_data(panel);
    GList *children = gtk_container_get_children(GTK_CONTAINER(d->suggestion_flow));
    for (GList *l = children; l; l = l->next) {
        gtk_widget_destroy(GTK_WIDGET(l->data));
    }
    g_list_free(children);
    gtk_widget_set_visible(d->suggestion_box, FALSE);
}

void
fsearch_filter_panel_widget_set_suggestions(GtkWidget *panel,
                                            const char *message,
                                            const char *const *ids,
                                            const char *const *labels,
                                            guint n) {
    PanelData *d = get_data(panel);
    fsearch_filter_panel_widget_clear_suggestions(panel);
    gtk_label_set_text(GTK_LABEL(d->suggestion_label), message);
    for (guint i = 0; i < n; ++i) {
        GtkWidget *button = gtk_button_new_with_label(labels[i]);
        gtk_widget_set_tooltip_text(button, _("Remove this condition and search again"));
        g_object_set_data_full(G_OBJECT(button), "condition-id", g_strdup(ids[i]), g_free);
        g_signal_connect(button, "clicked", G_CALLBACK(on_suggestion_clicked), d);
        gtk_widget_show(button);
        gtk_flow_box_insert(GTK_FLOW_BOX(d->suggestion_flow), button, -1);
    }
    gtk_widget_set_visible(d->suggestion_flow, n > 0);
    gtk_widget_set_visible(d->suggestion_box, TRUE);
}
