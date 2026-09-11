// A minimal StatusNotifierItem over GDBus.
//
// GTK4 removed GtkStatusIcon and ships no tray API of any kind. Ubuntu 24.04
// has no GTK4 tray library either (libayatana-appindicator3 is GTK3; libdbusmenu
// is GTK3; there is no gtk4 binding packaged), so a GTK4 app has to speak the
// protocol itself. This is that: object on /StatusNotifierItem exporting
// org.kde.StatusNotifierItem's properties, then RegisterStatusNotifierItem on
// org.kde.StatusNotifierWatcher.
//
// It is deliberately the smallest thing a panel will accept -- no menu
// (ItemIsMenu=false, so the panel sends Activate instead of showing a
// com.canonical.dbusmenu, which would be another ~400 lines). Count these
// lines against QSystemTrayIcon's three in the report.
#include "screens.h"

#include <gio/gio.h>

static const char introspection_xml[] =
    "<node>"
    "  <interface name='org.kde.StatusNotifierItem'>"
    "    <property name='Category' type='s' access='read'/>"
    "    <property name='Id' type='s' access='read'/>"
    "    <property name='Title' type='s' access='read'/>"
    "    <property name='Status' type='s' access='read'/>"
    "    <property name='IconName' type='s' access='read'/>"
    "    <property name='ToolTip' type='(sa(iiay)ss)' access='read'/>"
    "    <property name='ItemIsMenu' type='b' access='read'/>"
    "    <method name='Activate'>"
    "      <arg name='x' type='i' direction='in'/>"
    "      <arg name='y' type='i' direction='in'/>"
    "    </method>"
    "    <signal name='NewStatus'><arg name='status' type='s'/></signal>"
    "  </interface>"
    "</node>";

static void handle_method(GDBusConnection *conn, const char *sender, const char *path,
                          const char *iface, const char *method, GVariant *params,
                          GDBusMethodInvocation *inv, gpointer user_data)
{
    if (g_strcmp0(method, "Activate") == 0)
        g_print("tray: Activate\n");
    g_dbus_method_invocation_return_value(inv, NULL);
}

static GVariant *handle_get_property(GDBusConnection *conn, const char *sender,
                                     const char *path, const char *iface,
                                     const char *prop, GError **error, gpointer user_data)
{
    if (g_strcmp0(prop, "Category") == 0) return g_variant_new_string("ApplicationStatus");
    if (g_strcmp0(prop, "Id") == 0) return g_variant_new_string("vorssaint-gtk-spike");
    if (g_strcmp0(prop, "Title") == 0) return g_variant_new_string("Vorssaint (WP-01 GTK spike)");
    if (g_strcmp0(prop, "Status") == 0) return g_variant_new_string("Active");
    if (g_strcmp0(prop, "IconName") == 0) return g_variant_new_string("utilities-system-monitor");
    if (g_strcmp0(prop, "ItemIsMenu") == 0) return g_variant_new_boolean(FALSE);
    if (g_strcmp0(prop, "ToolTip") == 0)
        return g_variant_new("(sa(iiay)ss)", "utilities-system-monitor", NULL,
                             "Vorssaint (WP-01 GTK spike)", "");
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "no property %s", prop);
    return NULL;
}

static const GDBusInterfaceVTable vtable = { handle_method, handle_get_property, NULL, { 0 } };

gboolean sni_register(void)
{
    GError *err = NULL;
    GDBusConnection *conn = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
    if (!conn) {
        g_printerr("tray: no session bus: %s\n", err->message);
        g_clear_error(&err);
        return FALSE;
    }

    GDBusNodeInfo *info = g_dbus_node_info_new_for_xml(introspection_xml, &err);
    if (!info) {
        g_printerr("tray: bad introspection: %s\n", err->message);
        g_clear_error(&err);
        return FALSE;
    }

    guint reg = g_dbus_connection_register_object(conn, "/StatusNotifierItem",
                                                  info->interfaces[0], &vtable,
                                                  NULL, NULL, &err);
    if (reg == 0) {
        g_printerr("tray: register_object failed: %s\n", err->message);
        g_clear_error(&err);
        return FALSE;
    }

    // The watcher wants either our well-known name or our unique name; the
    // unique name is what every panel accepts and needs no name request.
    const char *unique = g_dbus_connection_get_unique_name(conn);
    GVariant *reply = g_dbus_connection_call_sync(
        conn, "org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher",
        "org.kde.StatusNotifierWatcher", "RegisterStatusNotifierItem",
        g_variant_new("(s)", unique), NULL, G_DBUS_CALL_FLAGS_NONE, 3000, NULL, &err);

    if (!reply) {
        g_printerr("tray: no StatusNotifierWatcher: %s\n", err->message);
        g_clear_error(&err);
        g_print("tray: registered=false\n");
        return FALSE;
    }
    g_variant_unref(reply);
    g_print("tray: registered=true as %s\n", unique);
    return TRUE;
}
