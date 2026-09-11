// Screen 2: preferences, using libadwaita's AdwPreferencesPage rows. The
// toggle, the slider and the shortcut recorder each send a command and render
// what the snapshot sends back.
#include "screens.h"

#include <adwaita.h>

typedef struct {
    CoreModel *settings;
    GtkWidget *sw;
    GtkWidget *scale;
    GtkWidget *shortcut_btn;
    GtkWidget *snapshot_label;
    gboolean recording;
    gboolean applying; // guards the snapshot -> widget write from re-emitting
} PrefsState;

static void on_switch(GtkSwitch *sw, gboolean active, gpointer data)
{
    PrefsState *st = data;
    if (st->applying) return;
    char *json = g_strdup_printf("{\"set\":{\"key\":\"toggle\",\"value\":%s}}",
                                 active ? "true" : "false");
    core_model_invoke(st->settings, json);
    g_free(json);
}

static void on_scale(GtkRange *range, gpointer data)
{
    PrefsState *st = data;
    if (st->applying) return;
    char *json = g_strdup_printf("{\"set\":{\"key\":\"slider\",\"value\":%d}}",
                                 (int)gtk_range_get_value(range));
    core_model_invoke(st->settings, json);
    g_free(json);
}

static gboolean on_key(GtkEventControllerKey *ctrl, guint keyval, guint code,
                       GdkModifierType mods, gpointer data)
{
    PrefsState *st = data;
    if (!st->recording) return FALSE;
    if (keyval == GDK_KEY_Control_L || keyval == GDK_KEY_Control_R
        || keyval == GDK_KEY_Alt_L || keyval == GDK_KEY_Alt_R
        || keyval == GDK_KEY_Shift_L || keyval == GDK_KEY_Shift_R
        || keyval == GDK_KEY_Super_L || keyval == GDK_KEY_Super_R)
        return TRUE;

    GString *chord = g_string_new(NULL);
    if (mods & GDK_CONTROL_MASK) g_string_append(chord, "Ctrl+");
    if (mods & GDK_ALT_MASK) g_string_append(chord, "Alt+");
    if (mods & GDK_SHIFT_MASK) g_string_append(chord, "Shift+");
    if (mods & GDK_SUPER_MASK) g_string_append(chord, "Super+");
    g_string_append(chord, gdk_keyval_name(gdk_keyval_to_upper(keyval)));

    char *json = g_strdup_printf("{\"recordShortcut\":\"%s\"}", chord->str);
    core_model_invoke(st->settings, json);
    g_free(json);
    g_string_free(chord, TRUE);
    st->recording = FALSE;
    return TRUE;
}

static void on_record_clicked(GtkButton *btn, gpointer data)
{
    PrefsState *st = data;
    st->recording = TRUE;
    gtk_button_set_label(btn, "Press a chord...");
    gtk_widget_grab_focus(GTK_WIDGET(btn));
}

static void on_settings_changed(CoreModel *model, gpointer data)
{
    PrefsState *st = data;
    st->applying = TRUE;
    gtk_switch_set_active(GTK_SWITCH(st->sw), core_model_get_bool(model, "toggle", FALSE));
    gtk_range_set_value(GTK_RANGE(st->scale), core_model_get_double(model, "slider", 0));
    gtk_button_set_label(GTK_BUTTON(st->shortcut_btn),
                         core_model_get_string(model, "shortcut", "none"));
    char *snap = g_strdup_printf("toggle=%s  slider=%.0f  shortcut=%s  selection=%s",
                                 core_model_get_bool(model, "toggle", FALSE) ? "true" : "false",
                                 core_model_get_double(model, "slider", 0),
                                 core_model_get_string(model, "shortcut", "none"),
                                 core_model_get_string(model, "selection", "none"));
    gtk_label_set_text(GTK_LABEL(st->snapshot_label), snap);
    g_free(snap);
    st->applying = FALSE;
}

// Headless runs have no keyboard; drive the recorder once so the screenshot
// shows a real round trip through vs_command.
static gboolean demo_record(gpointer data)
{
    core_model_invoke(((PrefsState *)data)->settings, "{\"recordShortcut\":\"Ctrl+Alt+K\"}");
    return G_SOURCE_REMOVE;
}

GtkWidget *build_preferences(GtkApplication *app)
{
    PrefsState *st = g_new0(PrefsState, 1);
    st->settings = core_model_new("settings");

    GtkWidget *win = adw_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(win), "Vorssaint preferences");
    gtk_window_set_default_size(GTK_WINDOW(win), 480, 320);

    GtkWidget *page = adw_preferences_page_new();
    GtkWidget *group = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group), "Preferences");

    AdwActionRow *row1 = ADW_ACTION_ROW(adw_action_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row1), "Launch at login");
    st->sw = gtk_switch_new();
    gtk_widget_set_valign(st->sw, GTK_ALIGN_CENTER);
    adw_action_row_add_suffix(row1, st->sw);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), GTK_WIDGET(row1));

    AdwActionRow *row2 = ADW_ACTION_ROW(adw_action_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row2), "Update interval");
    st->scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
    gtk_widget_set_size_request(st->scale, 200, -1);
    gtk_widget_set_valign(st->scale, GTK_ALIGN_CENTER);
    gtk_scale_set_draw_value(GTK_SCALE(st->scale), TRUE);
    adw_action_row_add_suffix(row2, st->scale);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), GTK_WIDGET(row2));

    AdwActionRow *row3 = ADW_ACTION_ROW(adw_action_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row3), "Shortcut");
    st->shortcut_btn = gtk_button_new_with_label("none");
    gtk_widget_set_valign(st->shortcut_btn, GTK_ALIGN_CENTER);
    adw_action_row_add_suffix(row3, st->shortcut_btn);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), GTK_WIDGET(row3));

    st->snapshot_label = gtk_label_new("");
    gtk_widget_add_css_class(st->snapshot_label, "dim-label");
    gtk_label_set_wrap(GTK_LABEL(st->snapshot_label), TRUE);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), st->snapshot_label);

    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(group));

    GtkWidget *toolbar = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), adw_header_bar_new());
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), page);
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(win), toolbar);

    GtkEventController *keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), st);
    gtk_widget_add_controller(win, keys);

    g_signal_connect(st->sw, "state-set", G_CALLBACK(on_switch), st);
    g_signal_connect(st->scale, "value-changed", G_CALLBACK(on_scale), st);
    g_signal_connect(st->shortcut_btn, "clicked", G_CALLBACK(on_record_clicked), st);
    g_signal_connect(st->settings, "changed", G_CALLBACK(on_settings_changed), st);
    on_settings_changed(st->settings, st);
    g_timeout_add(900, demo_record, st);
    return win;
}
