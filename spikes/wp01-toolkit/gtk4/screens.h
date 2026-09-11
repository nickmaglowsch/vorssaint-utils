#pragma once

#include <gtk/gtk.h>

#include "core_model.h"

GtkWidget *build_panel(GtkApplication *app);
GtkWidget *build_preferences(GtkApplication *app);
GtkWidget *build_overlay(GtkApplication *app, gboolean demo_drag);

// Registers a StatusNotifierItem on the session bus. See sni.c for why this
// has to be hand-written for GTK4.
gboolean sni_register(void);
