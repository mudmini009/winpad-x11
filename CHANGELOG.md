# Changelog

All notable changes to `winpad-x11` are documented in this file.

## [1.4.0] - 2026-11-26

### Added
- **3-finger Tap → `Super` / Search (`three_finger_tap = super`):** A quick 3-finger tap sends `Super_L` (opening Whisker Menu / Search on Xubuntu, or any custom `three_finger_tap_cmd`), automatically muting touchpad Middle Click (`Button 2`) while active and restoring it cleanly on exit or `three_finger_tap = off`.
- **1-finger Double-Tap / Click-Drag Edge Auto-Slide (`edge_drag_slide = true`):** Reaching the outer border/corner of the touchpad while dragging continues gliding the cursor smoothly at 60fps via `XTestFakeRelativeMotionEvent` until you pull inward or lift your finger (`0%` idle CPU).

### Changed
- **Windows-authentic 4-finger desktop switching:** Changed default 4-finger horizontal behavior (`four_finger_scrub = false`) to switch **one Virtual Desktop per swipe** (`D1 → D2` per swipe), matching Windows 10/11 Precision Touchpad ergonomics. Set `four_finger_scrub = true` in `~/.config/winpad-gestures.conf` to opt back into continuous multi-desktop scrubbing.
- **Boundary-clamped 3-finger `Alt+Tab` & Task View scrubbing:** Both `Alt+Tab` scrubbing (via `_NET_CLIENT_LIST` window filtering) and Task View Left/Right card selection now **stop cleanly at the first and last window** instead of wrapping around. Also handles starting `Alt+Tab` when no normal window is focused (e.g. desktop / Show Desktop).

### Improved
- **Upgraded Task View (`winpad-taskview`) visuals & speed:**
  - Added `on_draw_card()` with a lightweight 3-ring cyan-to-violet outer aura, radial spotlight, luminous gradient border, top specular edge, and top accent pill bar on the selected window card.
  - Added an `Active` pill badge on the previously focused window, frosted pill badges on minimized/off-workspace icon cards, header accent bar + live search hint chip, and theme-independent `6x6` Cairo indicator dots on the bottom Virtual Desktops dock.
  - Added cached ambient backdrop rendering (`g_bg_cache`), skipped redundant `XComposite` round-trips for minimized/off-workspace windows at startup, added lazy `XComposite` thumbnail capture on desktop switches while Task View is open, added `libwnck 43+` (`WnckHandle`) compatibility alongside `libwnck < 43`, and fixed middle-click card removal.
- **Daemon & OSD polish:** Added 15 ms frame-rate throttling + time-compensated glide on the Fluent OSD, `4 -> 3` finger liftoff anti-misfire lock (`four_f_locked`), `SYN_DROPPED` recovery, and clean default signal restoration in forked children.

## [1.3.1] - 2026-11-26
- Double-buffered zero-flicker ARGB OSD, per-monitor Task View placement (`gtk_window_fullscreen_on_monitor`), universal `scripts/setup.sh`, and `make uninstall`.

## [1.3.0] - 2026-11-26
- Continuous 4-finger desktop scrub, fast multi-finger landing merge (`55 ms`), and 60fps sliding Fluent OSD.

## [1.2.0] - 2026-11-26
- Primary-monitor 32-bit ARGB Fluent Desktop OSD via XRandR and Windows 11-aligned 3/4-finger default directions.

## [1.1.0] - 2026-11-26
- Windows 11-style bottom Virtual Desktops dock in `winpad-taskview` and 3-vs-4 finger settle window.

## [1.0.0] - 2026-11-26
- Initial release: pure C `evdev` + `X11`/`XTest` gesture daemon (`winpad-gestures`) and lightweight live `XComposite` Task View (`winpad-taskview`).
