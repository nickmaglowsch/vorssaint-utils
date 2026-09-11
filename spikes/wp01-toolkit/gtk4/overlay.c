// Screen 3: the capture overlay. Transparent fullscreen surface with a drag
// rectangle. GTK4 has no per-window "transparent background" switch: the
// window must be given a CSS background of `transparent` and everything is
// painted in one draw func.
#include "screens.h"

#include <adwaita.h>

typedef struct {
    CoreModel *settings;
    GtkWidget *area;
    double ox, oy, ow, oh;
    gboolean dragging;
} OverlayState;

static void draw_overlay(GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer data)
{
    OverlayState *st = data;

    cairo_set_source_rgba(cr, 0, 0, 0, 0.4);
    cairo_rectangle(cr, 0, 0, w, h);
    cairo_fill(cr);

    cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 16);
    cairo_set_source_rgb(cr, 1, 1, 1);
    char *hint = g_strdup_printf("Drag to select  -  core says: %s",
                                 core_model_get_string(st->settings, "selection", "none"));
    cairo_move_to(cr, 40, 50);
    cairo_show_text(cr, hint);
    g_free(hint);

    if (st->ow <= 0)
        return;

    cairo_set_source_rgba(cr, 1, 1, 1, 0.13);
    cairo_rectangle(cr, st->ox, st->oy, st->ow, st->oh);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, 0.290, 0.565, 0.851);
    cairo_set_line_width(cr, 2);
    cairo_rectangle(cr, st->ox, st->oy, st->ow, st->oh);
    cairo_stroke(cr);

    char *label = g_strdup_printf("%d,%d  %dx%d", (int)st->ox, (int)st->oy,
                                  (int)st->ow, (int)st->oh);
    cairo_text_extents_t ext;
    cairo_set_font_size(cr, 12);
    cairo_text_extents(cr, label, &ext);
    cairo_rectangle(cr, st->ox, st->oy - 26, ext.width + 12, 22);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_move_to(cr, st->ox + 6, st->oy - 10);
    cairo_show_text(cr, label);
    g_free(label);
}

static void begin_at(OverlayState *st, double x, double y)
{
    st->ox = x; st->oy = y; st->ow = 0; st->oh = 0; st->dragging = TRUE;
}

static void extend_to(OverlayState *st, double x, double y)
{
    if (!st->dragging) return;
    st->ow = ABS(x - st->ox); st->oh = ABS(y - st->oy);
    gtk_widget_queue_draw(st->area);
}

static void finish(OverlayState *st)
{
    st->dragging = FALSE;
    char *rect = g_strdup_printf("%d,%d,%d,%d", (int)st->ox, (int)st->oy,
                                 (int)st->ow, (int)st->oh);
    g_print("selected rect: %s\n", rect);
    char *json = g_strdup_printf("{\"set\":{\"key\":\"selection\",\"value\":\"%s\"}}", rect);
    core_model_invoke(st->settings, json);
    g_free(json);
    g_free(rect);
    gtk_widget_queue_draw(st->area);
}

static void on_drag_begin(GtkGestureDrag *g, double x, double y, gpointer d)
{
    begin_at(d, x, y);
}
static void on_drag_update(GtkGestureDrag *g, double dx, double dy, gpointer d)
{
    OverlayState *st = d;
    extend_to(st, st->ox + dx, st->oy + dy);
}
static void on_drag_end(GtkGestureDrag *g, double dx, double dy, gpointer d)
{
    finish(d);
}

// Headless runs have no pointer; replay one drag through the same functions
// the gesture calls, so the screenshot is of real state.
static gboolean demo_drag_cb(gpointer data)
{
    OverlayState *st = data;
    begin_at(st, 220, 180);
    extend_to(st, 760, 520);
    finish(st);
    return G_SOURCE_REMOVE;
}

GtkWidget *build_overlay(GtkApplication *app, gboolean demo_drag)
{
    OverlayState *st = g_new0(OverlayState, 1);
    st->settings = core_model_new("settings");

    GtkWidget *win = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(win), "Vorssaint overlay");
    gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
    gtk_window_fullscreen(GTK_WINDOW(win));

    // The only way to get a transparent GTK4 window: CSS on the widget.
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css, "window, window > * { background: transparent; }");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
                                               GTK_STYLE_PROVIDER(css),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    st->area = gtk_drawing_area_new();
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(st->area), draw_overlay, st, NULL);
    gtk_window_set_child(GTK_WINDOW(win), st->area);

    GtkGesture *drag = gtk_gesture_drag_new();
    g_signal_connect(drag, "drag-begin", G_CALLBACK(on_drag_begin), st);
    g_signal_connect(drag, "drag-update", G_CALLBACK(on_drag_update), st);
    g_signal_connect(drag, "drag-end", G_CALLBACK(on_drag_end), st);
    gtk_widget_add_controller(st->area, GTK_EVENT_CONTROLLER(drag));

    if (demo_drag)
        g_timeout_add(700, demo_drag_cb, st);
    return win;
}
