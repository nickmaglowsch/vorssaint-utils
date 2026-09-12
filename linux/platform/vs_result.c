/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Vorssaint
 *
 * The one piece of vorssaint_platform.h that belongs to no single concern.
 * Every backend library links this object, so a binary that uses two of them
 * (the Swift side will use all of them) gets one definition rather than one
 * per concern.
 *
 * The window-specific wording of NOT_FOUND and NO_BACKEND that WP-C1 wrote
 * inside vs_window.c is generalised here; see linux/platform/capture/README.md.
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
