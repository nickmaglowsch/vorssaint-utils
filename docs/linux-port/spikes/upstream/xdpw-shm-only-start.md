# screencast fails on any wlroots session without a DMA-BUF capable renderer

**Draft for upstream (`xdg-desktop-portal-wlr`). Not submitted.** Posting to
another project's tracker is the repository owner's call; this file is the text
ready to go.

## Affected version

- `xdg-desktop-portal-wlr` **0.7.1**, as packaged in Ubuntu 24.04
  (`0.7.1-1build2`).
- Upstream source: `src/screencast/screencast.c:199-204`, in
  `start_screencast()`.

## Summary

`start_screencast()` treats a missing DMA-BUF format as fatal whenever the
compositor advertises `zwlr_screencopy_manager_v1` version 3 or newer. Any
wlroots compositor running a renderer that cannot export DMA-BUFs — the pixman
software renderer, or any machine with no `/dev/dri` — still advertises
screencopy v3 but never sends the `linux_dmabuf` event, so every `Start()` on
the ScreenCast portal fails, even though the shared-memory path is fully
functional.

## Reproduction

Needs only sway headless; no GPU, no display, no login session.

```sh
export XDG_RUNTIME_DIR=/tmp/xdpw-repro; mkdir -p $XDG_RUNTIME_DIR; chmod 700 $XDG_RUNTIME_DIR
eval "$(dbus-daemon --session --print-address --fork)" 2>/dev/null || \
  export DBUS_SESSION_BUS_ADDRESS=$(dbus-daemon --session --print-address --fork)

mkdir -p ~/.config/xdg-desktop-portal-wlr
cat > ~/.config/xdg-desktop-portal-wlr/config <<'EOF'
[screencast]
output_name=HEADLESS-1
chooser_type=none
EOF

printf 'output HEADLESS-1 mode 1280x720@60Hz\nxwayland disable\n' > /tmp/sway.cfg
WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1 \
  XDG_CURRENT_DESKTOP=sway WAYLAND_DISPLAY=wayland-1 sway -c /tmp/sway.cfg &

WAYLAND_DISPLAY=wayland-1 XDG_CURRENT_DESKTOP=sway \
  /usr/libexec/xdg-desktop-portal-wlr -l DEBUG &
/usr/libexec/xdg-desktop-portal &
```

Then drive the portal: `CreateSession` → `SelectSources` (`types=1`,
`persist_mode=2`) → `Start`. Any ScreenCast client does; we used a small
GDBus CLI.

## Expected

`Start()` returns response 0 and a PipeWire node carrying SHM buffers, as it
does on a wlroots session whose compositor advertises screencopy v2.

## Actual

```
SelectSources_response=0
Start_response=2

# xdg-desktop-portal-wlr log
wlroots: |-- registered to interface zwlr_screencopy_manager_v1 (Version 3)
wlroots: unable to receive a valid format from wlr_screencopy
```

## Why the SHM path is in fact fine

The guard is the only thing objecting. Roughly 40 lines further down,
`build_formats()` (`src/screencast/pipewire_screencast.c`) already falls back to
an SHM-only `EnumFormat` when no modifier list can be built:

```c
if (!cast->avoid_dmabufs &&
        build_modifierlist(cast, ..., &modifiers, &modifier_count) && modifier_count > 0) {
        param_count = 2;   /* dmabuf + shm */
        ...
} else {
        param_count = 1;   /* shm only */
        ...
}
```

and the whole buffer path handles `buffer_type == WL_SHM`. With the guard
relaxed, the cast negotiates and runs.

## Proposed fix

Make the DMA-BUF format optional; keep the SHM format required.

```diff
--- a/src/screencast/screencast.c
+++ b/src/screencast/screencast.c
@@ -196,9 +196,7 @@ static int start_screencast(struct xdpw_screencast_instance *cast) {
 	wl_display_dispatch(cast->ctx->state->wl_display);
 	wl_display_roundtrip(cast->ctx->state->wl_display);

-	if (cast->screencopy_frame_info[WL_SHM].format == DRM_FORMAT_INVALID ||
-			(cast->ctx->state->screencast_version >= 3 &&
-			 cast->screencopy_frame_info[DMABUF].format == DRM_FORMAT_INVALID)) {
+	if (cast->screencopy_frame_info[WL_SHM].format == DRM_FORMAT_INVALID) {
 		logprint(INFO, "wlroots: unable to receive a valid format from wlr_screencopy");
 		return -1;
 	}
```

## Evidence that the fix is sufficient

Same compositor, same config, only the backend binary swapped:

```
### stock 0.7.1
SelectSources_response=0
Start_response=2
xdpw: wlroots: unable to receive a valid format from wlr_screencopy

### same source + the diff above
SelectSources_response=0
Start_response=0
frames=15 frames_with_header=15 dropped=0
xdpw: pipewire: buffer_type: 0 (4)          # 0 == WL_SHM
```

With the patched build the cast then ran normally: 1280x720 BGRx, 29.5 fps
against `max_fps=30`, 292–294 frames per 10 s run, zero dropped buffers,
`spa_meta_header` present on every frame.

## Impact

Affects real users, not only containers: a wlroots session on the pixman
renderer, a VM without virtio-gpu, a broken or blacklisted GPU driver, and some
remote/VNC setups all hit it. The failure surfaces to the application as a bare
`Start` response 2 with no diagnostic.
