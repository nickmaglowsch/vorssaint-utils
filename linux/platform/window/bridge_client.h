/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Vorssaint
 *
 * `org.vorssaint.WindowBridge`: the one D-Bus interface both compositor
 * bridges speak, so the client side is written once.
 *
 *   GNOME   the vorssaint-bridge Shell extension (WP-C2) owns
 *           org.vorssaint.WindowBridge at /org/vorssaint/WindowBridge and
 *           implements every member below. GJS can export a D-Bus object, so
 *           the extension is the service and this file is the whole client.
 *
 *   KWin    KWin's scripting engine can *call* D-Bus (`callDBus`) but cannot
 *           own a name, so a KWin script cannot be the service. backend_kwin.c
 *           therefore owns org.vorssaint.KWinBridge itself as a *sink* the
 *           script pushes into, and sends requests the other way by loading
 *           one-shot scripts through org.kde.KWin /Scripting. Only the window
 *           encoding below is shared with GNOME; see backend_kwin.c.
 *
 * Window encoding, used by both: a(tsssiiiiiuis)
 *   t  id          u  flags (vs_window_flag)
 *   s  app_id      i  workspace
 *   s  app_name    s  output
 *   s  title
 *   i  pid, x, y, width, height
 */

#ifndef VS_BRIDGE_CLIENT_H
#define VS_BRIDGE_CLIENT_H

#include <systemd/sd-bus.h>

#include "vs_window_internal.h"

#define VS_BRIDGE_WINDOW_SIGNATURE "tsssiiiiiuis"
#define VS_BRIDGE_WINDOW_ARRAY_SIGNATURE "a(" VS_BRIDGE_WINDOW_SIGNATURE ")"

#define VS_BRIDGE_INTERFACE "org.vorssaint.WindowBridge"
#define VS_BRIDGE_PATH "/org/vorssaint/WindowBridge"
#define VS_BRIDGE_GNOME_SERVICE "org.vorssaint.WindowBridge"

/** Read one `a(tsssiiiiiuis)` array out of a message. */
int vs_bridge_read_windows(sd_bus_message *message, vs_window_info **windows_out, size_t *count_out);

/** True when `service` currently has an owner on the session bus. */
bool vs_bridge_name_has_owner(sd_bus *bus, const char *service);

#endif /* VS_BRIDGE_CLIENT_H */
