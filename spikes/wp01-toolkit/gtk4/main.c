// WP-01 GTK4/libadwaita candidate. Same three screens, same bridge stub, same
// flags as the Qt candidate: --screen panel|prefs|overlay, --demo-drag,
// --quit-after <ms>.
#include "screens.h"

#include <adwaita.h>
#include <stdlib.h>
#include <string.h>

static const char *g_screen = "panel";
static gboolean g_demo_drag = FALSE;
static int g_quit_after = 0;

static gboolean quit_cb(gpointer app)
{
    g_application_quit(G_APPLICATION(app));
    return G_SOURCE_REMOVE;
}

static void on_activate(GtkApplication *app, gpointer user_data)
{
    sni_register();

    GtkWidget *win;
    if (g_strcmp0(g_screen, "prefs") == 0)
        win = build_preferences(app);
    else if (g_strcmp0(g_screen, "overlay") == 0)
        win = build_overlay(app, g_demo_drag);
    else
        win = build_panel(app);

    gtk_window_present(GTK_WINDOW(win));

    if (g_quit_after > 0)
        g_timeout_add(g_quit_after, quit_cb, app);
}

int main(int argc, char *argv[])
{
    for (int i = 1; i < argc; i++) {
        if (g_strcmp0(argv[i], "--screen") == 0 && i + 1 < argc) g_screen = argv[++i];
        else if (g_strcmp0(argv[i], "--demo-drag") == 0) g_demo_drag = TRUE;
        else if (g_strcmp0(argv[i], "--quit-after") == 0 && i + 1 < argc)
            g_quit_after = atoi(argv[++i]);
    }

    AdwApplication *app = adw_application_new("dev.vorssaint.GtkSpike",
                                              G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    // GTK parses argv itself and would choke on our flags; hand it none.
    int status = g_application_run(G_APPLICATION(app), 1, argv);
    g_object_unref(app);
    return status;
}
