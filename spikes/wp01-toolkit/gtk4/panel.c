// Screen 1: panel with the live CPU sparkline, drawn on a GtkDrawingArea with
// cairo. GTK4 has no Canvas element and no declarative binding, so the redraw
// is wired by hand: the model's "changed" signal queues a draw.
#include "screens.h"

#include <adwaita.h>

typedef struct {
    CoreModel *metrics;
    GtkWidget *value;
} PanelState;

static void draw_spark(GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer data)
{
    PanelState *st = data;
    double hist[VS_HISTORY_MAX];
    guint n = core_model_copy_history(st->metrics, hist, VS_HISTORY_MAX);

    cairo_set_source_rgb(cr, 0.169, 0.188, 0.231);
    cairo_set_line_width(cr, 1);
    for (int g = 0; g <= 4; g++) {
        double gy = (double)h * g / 4.0;
        cairo_move_to(cr, 0, gy);
        cairo_line_to(cr, w, gy);
    }
    cairo_stroke(cr);
    if (n < 2)
        return;

    double step = (double)w / (n - 1);
    cairo_move_to(cr, 0, h);
    for (guint i = 0; i < n; i++)
        cairo_line_to(cr, i * step, h - hist[i] / 100.0 * h);
    cairo_line_to(cr, (n - 1) * step, h);
    cairo_close_path(cr);
    cairo_set_source_rgb(cr, 0.114, 0.227, 0.322);
    cairo_fill(cr);

    for (guint i = 0; i < n; i++) {
        double x = i * step, y = h - hist[i] / 100.0 * h;
        i ? cairo_line_to(cr, x, y) : cairo_move_to(cr, x, y);
    }
    cairo_set_source_rgb(cr, 0.290, 0.565, 0.851);
    cairo_set_line_width(cr, 2);
    cairo_stroke(cr);
}

static void on_metrics_changed(CoreModel *model, gpointer data)
{
    PanelState *st = data;
    char *text = g_strdup_printf("%.1f %%", core_model_get_double(model, "cpu", 0));
    gtk_label_set_text(GTK_LABEL(st->value), text);
    g_free(text);
    gtk_widget_queue_draw(GTK_WIDGET(g_object_get_data(G_OBJECT(model), "area")));
}

GtkWidget *build_panel(GtkApplication *app)
{
    PanelState *st = g_new0(PanelState, 1);
    st->metrics = core_model_new("metrics");

    GtkWidget *win = adw_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(win), "Vorssaint panel");
    gtk_window_set_default_size(GTK_WINDOW(win), 340, 200);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(box, 16);
    gtk_widget_set_margin_end(box, 16);
    gtk_widget_set_margin_top(box, 14);
    gtk_widget_set_margin_bottom(box, 14);

    GtkWidget *heading = gtk_label_new("CPU");
    gtk_widget_add_css_class(heading, "dim-label");
    gtk_widget_set_halign(heading, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(box), heading);

    st->value = gtk_label_new("-- %");
    gtk_widget_add_css_class(st->value, "title-1");
    gtk_widget_set_halign(st->value, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(box), st->value);

    GtkWidget *area = gtk_drawing_area_new();
    gtk_widget_set_size_request(area, -1, 84);
    gtk_widget_set_vexpand(area, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area), draw_spark, st, NULL);
    gtk_box_append(GTK_BOX(box), area);
    g_object_set_data(G_OBJECT(st->metrics), "area", area);

    adw_application_window_set_content(ADW_APPLICATION_WINDOW(win), box);
    g_signal_connect(st->metrics, "changed", G_CALLBACK(on_metrics_changed), st);
    on_metrics_changed(st->metrics, st);
    return win;
}
