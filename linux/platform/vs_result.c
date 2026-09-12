/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Vorssaint
 *
 * The one piece of the header that is not a section: turning a `vs_result`
 * into something a log line or a CLI harness can print.
 *
 * It lives here rather than in one concern's library because it belongs to all
 * of them. It arrived with the window section (WP-C1) and moved out when the
 * audio section (WP-A5) became the second caller; the wordings lost their
 * window-specific phrasing in the same move, since "no such window" is wrong
 * when the thing that was not found is a sink.
 */

#include "vorssaint_platform.h"

const char *vs_result_string(int result)
{
    switch (result) {
    case VS_OK: return "ok";
    case VS_ERR_UNSUPPORTED: return "unsupported by this backend";
    case VS_ERR_NOT_FOUND: return "no such object";
    case VS_ERR_BACKEND: return "backend refused or failed";
    case VS_ERR_TIMEOUT: return "backend timed out";
    case VS_ERR_NO_MEM: return "out of memory";
    case VS_ERR_INVALID: return "invalid argument";
    case VS_ERR_NO_BACKEND: return "no backend for this session";
    case VS_ERR_NOT_APPLIED: return "request acknowledged but state did not change";
    default: return "unknown error";
    }
}
