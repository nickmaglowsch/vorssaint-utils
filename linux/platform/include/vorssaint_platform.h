/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Vorssaint
 *
 * The C surface every Linux platform backend fills in.
 *
 * PLAN.md § 5 fixes the shape: Linux platform code is C under
 * `linux/platform`, one directory per concern, and the Swift `Platform`
 * protocols (WP-12) are mirrored here one-to-one rather than reimplemented in
 * Swift. `Sources/VorssaintLinux` wraps this header and nothing else.
 *
 * This file is a table of contents with one section per concern. A section
 * lands in the work package that writes its backend; the sections that have
 * no backend yet are listed below so the next author adds to this file
 * instead of starting a second one.
 *
 *   § audio      WP-A5   linux/platform/audio      mixer, output switching, mic mute
 *   § window     WP-A9   linux/platform/window     (not written yet)
 *   § capture    WP-B1   linux/platform/capture    (not written yet)
 *   § sensors    WP-A1   linux/platform/sensors    (not written yet)
 *   § power      WP-A2   linux/platform/power      (not written yet)
 *   § bluez      WP-A7   linux/platform/bluez      (not written yet)
 *   § portals    WP-24   linux/platform/portals    (not written yet)
 *   § helper     WP-S1   linux/helper/client       (client lives with the helper)
 *
 * Conventions every section follows:
 *
 * - One opaque handle per concern, opened once and closed at shutdown. The
 *   handle owns its own worker thread; every function here is safe to call
 *   from the caller's thread (the Qt main thread) unless a comment says
 *   otherwise.
 * - Integer returns are 0 on success and a negative errno on failure, so a
 *   caller can say *why* and not only *that* something failed.
 * - A backend declares what it can actually do through a capability bitmask
 *   read after open, never through a compile-time assumption about the
 *   running desktop (playbook: "Desktop support is declared, not assumed").
 * - Every write is read back before it is reported as done (playbook: "Read
 *   back after writing"); a function that cannot read back says so.
 */

#ifndef VORSSAINT_PLATFORM_H
#define VORSSAINT_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================
 * § audio -- WP-A5 -- linux/platform/audio, over libpipewire with a libpulse
 * fallback.
 *
 * Mirrors the macOS services this port replaces:
 *   AppVolumeMixer          per-app volume, per-app output routing, device list
 *   SoundOutputSwitcher     default output switching, headphone disconnect
 *   AudioInputDeviceManager input device list and default
 *   MicMuteService          mute every input, restore what was muted before
 *
 * WP-12's Swift `AudioGraph` protocol mirrors this section one-to-one; see
 * linux/platform/audio/README.md for the mapping and for what the macOS
 * mixer's percentages mean here.
 * ========================================================================== */

/* --- volume scale ---------------------------------------------------------
 *
 * Every volume in this section is LINEAR AMPLITUDE, where 1.0 is unity, i.e.
 * exactly the macOS mixer's convention (`AppVolumeMixer` runs 0...2 with 1.0
 * meaning untouched passthrough). A percentage P from the UI is P/100 here.
 *
 * This is deliberately *not* the number `wpctl` and the GNOME/KDE volume
 * sliders show. Those apply a cubic curve: `wpctl set-volume ID 0.5` writes a
 * linear channel volume of 0.125 (0.5 cubed). Converting is the UI's job, and
 * `vs_audio_linear_to_cubic` / `vs_audio_cubic_to_linear` below exist so it
 * is done in one place rather than guessed at each call site.
 */

/** Largest linear volume the mixer accepts: 150 %, above PipeWire's unity. */
#define VS_AUDIO_MAX_VOLUME 1.5f

/** What the panel shows as "100 %". */
#define VS_AUDIO_UNITY_VOLUME 1.0f

/** Capability bits, read with vs_audio_capabilities() after opening. */
enum {
    /** A pipewire daemon answered; the full backend is in use. */
    VS_AUDIO_CAP_HAS_PIPEWIRE = 1u << 0,
    /** The libpulse fallback is in use (PulseAudio-only host, or no pipewire). */
    VS_AUDIO_CAP_HAS_PULSE_FALLBACK = 1u << 1,
    /** Individual streams can be routed to a chosen sink, not just the default. */
    VS_AUDIO_CAP_CAN_ROUTE_PER_STREAM = 1u << 2,
    /** Volumes above 1.0 are honoured rather than clamped. */
    VS_AUDIO_CAP_CAN_BOOST_OVER_100 = 1u << 3,
    /** Change events are delivered to the callback; without this, poll. */
    VS_AUDIO_CAP_HAS_EVENTS = 1u << 4
};

/** What a node is, and the bit that selects it in a listing mask. */
typedef enum {
    VS_AUDIO_NODE_SINK = 1u << 0,          /**< media.class Audio/Sink */
    VS_AUDIO_NODE_SOURCE = 1u << 1,        /**< media.class Audio/Source */
    VS_AUDIO_NODE_STREAM_OUTPUT = 1u << 2, /**< Stream/Output/Audio: an app playing */
    VS_AUDIO_NODE_STREAM_INPUT = 1u << 3   /**< Stream/Input/Audio: an app recording */
} vs_audio_node_kind;

#define VS_AUDIO_NODE_ANY_DEVICE (VS_AUDIO_NODE_SINK | VS_AUDIO_NODE_SOURCE)
#define VS_AUDIO_NODE_ANY_STREAM \
    (VS_AUDIO_NODE_STREAM_OUTPUT | VS_AUDIO_NODE_STREAM_INPUT)
#define VS_AUDIO_NODE_ANY (VS_AUDIO_NODE_ANY_DEVICE | VS_AUDIO_NODE_ANY_STREAM)

/**
 * One device or one app stream.
 *
 * Strings are owned by the returned array and stay valid until it is freed
 * with vs_audio_free_nodes(). A string a backend could not supply is "" and
 * never NULL, so callers need no null checks; `pid` is -1 when unknown.
 */
typedef struct {
    /**
     * The handle every other function in this section takes. It is
     * PipeWire's `object.serial` (monotonic for the life of the daemon, so a
     * removed node's number is never handed to a later one -- unlike the
     * global id, which is recycled) and, in the pulse fallback, the
     * sink/source/sink-input index.
     */
    uint32_t id;
    /**
     * PipeWire's global id, which is what the metadata subject and the Link
     * objects use. 0 in the pulse fallback. Diagnostics and routing only:
     * do not persist it, it is recycled.
     */
    uint32_t pw_global_id;
    vs_audio_node_kind kind;

    const char *name;        /**< node.name -- stable, what to persist against */
    const char *description; /**< node.description -- what to show */
    const char *app_name;    /**< application.name (streams) */
    const char *icon_name;   /**< application.icon-name, an XDG icon name */
    const char *media_name;  /**< media.name -- what is playing right now */
    int32_t pid;             /**< application.process.id, or -1 */

    float volume; /**< linear, see the scale note above */
    bool mute;
    /** False for a node with no Props volume at all (rare; show no slider). */
    bool has_volume;
    /** This is the default sink (kind SINK) or default source (kind SOURCE). */
    bool is_default;

    /**
     * Streams only: the sink this stream is pinned to with `target.object`,
     * or 0 when it simply follows the default. This is what the mixer's
     * per-app output menu reads and writes.
     */
    uint32_t target_id;
    /**
     * Streams only: the `pw_global_id` of the node this stream's links
     * actually land on right now, or 0 when it is not linked. The difference
     * between this and `target_id` is how a routing request is verified:
     * asking is not the same as arriving.
     */
    uint32_t linked_pw_global_id;
} vs_audio_node;

/** Why the callback fired. */
typedef enum {
    /**
     * The graph changed in some way worth re-reading. Debounced: a burst of
     * adds, removes and param changes (a device appearing brings a dozen)
     * collapses into one of these, so the UI refreshes once.
     */
    VS_AUDIO_EVENT_CHANGED = 1,
    /** The default sink changed. `id` is the new default, 0 if there is none. */
    VS_AUDIO_EVENT_DEFAULT_SINK_CHANGED,
    /** The default source changed. */
    VS_AUDIO_EVENT_DEFAULT_SOURCE_CHANGED,
    /**
     * A sink went away while it was the default: headphones were unplugged,
     * a USB DAC was pulled, a Bluetooth device dropped. `id` is the sink that
     * left and `name` its node.name, because it is already gone from any
     * listing by the time this arrives. This is the event the macOS
     * "lower the volume when headphones disconnect" behaviour hangs on.
     */
    VS_AUDIO_EVENT_DEFAULT_SINK_DISCONNECTED
} vs_audio_event_kind;

typedef struct {
    vs_audio_event_kind kind;
    uint32_t id;      /**< the node concerned, or 0 */
    const char *name; /**< its node.name, or "", valid only during the call */
} vs_audio_event;

/**
 * Called from the backend's own worker thread, never from the caller's.
 * Do no work here beyond waking the UI thread: the PipeWire loop is blocked
 * for the duration and the graph cannot be read from inside it.
 */
typedef void (*vs_audio_event_fn)(const vs_audio_event *event, void *user_data);

typedef struct vs_audio vs_audio;

typedef struct {
    /** Milliseconds to coalesce graph churn into one CHANGED event. 0 = 80 ms. */
    unsigned debounce_ms;
    /**
     * Refuse to fall back to libpulse; open fails instead. For the harness
     * and the tests, which have to be able to say *which* backend they
     * measured.
     */
    bool require_pipewire;
    /** Skip PipeWire entirely and open the libpulse fallback. Tests only. */
    bool force_pulse;
    vs_audio_event_fn on_event;
    void *user_data;
} vs_audio_options;

/**
 * Connects to the audio server. PipeWire first; if `pw_context_connect`
 * fails and `require_pipewire` is false, libpulse is tried, and
 * VS_AUDIO_CAP_HAS_PULSE_FALLBACK is then set instead of
 * VS_AUDIO_CAP_HAS_PIPEWIRE.
 *
 * `options` may be NULL for the defaults. Returns NULL if no audio server
 * answered; `errno` is set (ENOENT: nothing to connect to).
 */
vs_audio *vs_audio_open(const vs_audio_options *options);
void vs_audio_close(vs_audio *audio);

/** The VS_AUDIO_CAP_* bits this connection actually has. */
uint32_t vs_audio_capabilities(const vs_audio *audio);

/** "pipewire" or "libpulse". For the UI's diagnostics line. */
const char *vs_audio_backend_name(const vs_audio *audio);

/**
 * Snapshots every node whose kind is in `kind_mask`.
 *
 * On success *out_nodes points at *out_count nodes to be released with
 * vs_audio_free_nodes(); a count of 0 comes with a NULL pointer. Returns 0 or
 * a negative errno.
 */
int vs_audio_list_nodes(vs_audio *audio, uint32_t kind_mask,
                        vs_audio_node **out_nodes, size_t *out_count);
void vs_audio_free_nodes(vs_audio_node *nodes, size_t count);

/**
 * Sets a node's linear volume, clamped to [0, VS_AUDIO_MAX_VOLUME], on every
 * channel, and waits for the server to report it back.
 *
 * Returns 0 when the read-back agrees within a tolerance, -EIO when the write
 * was accepted but the value that came back is a different one (which is what
 * a clamping server looks like), -ENOENT for an unknown id, -ETIMEDOUT when
 * nothing came back at all.
 */
int vs_audio_set_volume(vs_audio *audio, uint32_t id, float linear_volume);

/** Sets mute, with the same read-back contract as vs_audio_set_volume. */
int vs_audio_set_mute(vs_audio *audio, uint32_t id, bool mute);

/**
 * Pins one stream to one sink, the per-app output routing the macOS mixer
 * does by re-rendering through an aggregate device.
 *
 * `sink_id` of 0 clears the pin, putting the stream back on whatever the
 * default is. Returns 0 only once the stream's links have actually moved to
 * the requested sink (or, when clearing, once the pin is gone), -EIO if the
 * request was accepted but the links never followed, -ENOTSUP in the pulse
 * fallback.
 */
int vs_audio_route_stream(vs_audio *audio, uint32_t stream_id, uint32_t sink_id);

/**
 * Makes a sink the default one. Writes both `default.audio.sink` and
 * `default.configured.audio.sink` so the choice survives a restart, and
 * verifies by reading the metadata back.
 */
int vs_audio_set_default_sink(vs_audio *audio, uint32_t sink_id);

/**
 * Mutes every input (MicMuteService), or restores what was muted before.
 *
 * Turning it on records each source's mute state and mutes them all; turning
 * it off restores exactly those that this call muted, leaving alone any
 * source the user muted themselves in the meantime -- the same rule
 * MicMuteSupport.restoreTargets applies on macOS. `out_affected` may be NULL.
 */
int vs_audio_mute_all_inputs(vs_audio *audio, bool mute, size_t *out_affected);

/** Linear amplitude to the cubic number wpctl and the desktop sliders show. */
float vs_audio_linear_to_cubic(float linear);
/** The inverse: a cubic slider position to the linear volume used here. */
float vs_audio_cubic_to_linear(float cubic);

/* ==========================================================================
 * § window, § capture, § sensors, § power, § bluez, § portals
 *
 * Not written yet. Each lands with its work package (see the table at the top
 * of this file) as a section in this same shape: an opaque handle, an open
 * taking an options struct, a capability bitmask, snapshot listings whose
 * strings the listing owns, and writes that read back.
 * ========================================================================== */

#ifdef __cplusplus
}
#endif

#endif /* VORSSAINT_PLATFORM_H */
