#pragma once

#include <glib.h>
#include <gtk/gtk.h>
#include <stdbool.h>

G_BEGIN_DECLS

typedef void (*FsearchFilterPanelChangedFunc)(gpointer user_data);
typedef void (*FsearchFilterPanelAnalyzeFunc)(gpointer user_data);

// A panel with controls which narrow down a search (file type, extensions, size, date, properties of
// videos, ...). The values are turned into search syntax (see fsearch_filter_panel_query.h), which the
// window appends to the text of the search entry.
// `changed` is called (a moment after the last edit) whenever the filters change, `analyze` when the user
// asks to read the properties of the videos which weren't analyzed yet.
GtkWidget *
fsearch_filter_panel_widget_new(FsearchFilterPanelChangedFunc changed,
                                FsearchFilterPanelAnalyzeFunc analyze,
                                gpointer user_data);

void
fsearch_filter_panel_widget_set_revealed(GtkWidget *panel, gboolean revealed);

gboolean
fsearch_filter_panel_widget_get_revealed(GtkWidget *panel);

// Search syntax for the current values (free with g_free), NULL if no filter is set. With `include_media`
// false the conditions about the contents of video files are left out.
char *
fsearch_filter_panel_widget_get_query(GtkWidget *panel, bool include_media);

bool
fsearch_filter_panel_widget_has_media_conditions(GtkWidget *panel);

// "Smart matching" (see fsearch_smart_match.h) is switched in the panel, but applies to the text of the
// search entry. Setting the value doesn't trigger the `changed` callback.
gboolean
fsearch_filter_panel_widget_get_smart_matching(GtkWidget *panel);

void
fsearch_filter_panel_widget_set_smart_matching(GtkWidget *panel, gboolean enabled);

void
fsearch_filter_panel_widget_reset(GtkWidget *panel);

void
fsearch_filter_panel_widget_set_analyzing(GtkWidget *panel, bool analyzing);

G_END_DECLS
