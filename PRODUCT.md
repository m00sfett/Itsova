# Product

<!-- impeccable:product-schema 1 -->

## Platform

desktop (KDE Plasma on Wayland, native Qt 6 Widgets with the Breeze style; not one of web/ios/android)

## Users

First the author, a KDE Plasma power user with two monitors at different scales. Later other KDE Plasma users who find the project on GitHub. Their job: press Print in the middle of other work and get the screen saved instantly, then optionally cut it down to exactly what they need; and capture the same fixed areas, monitors or windows again and again at the moment a shortcut is pressed.

## Product Purpose

glimply replaces the Print key tool (since 0.2.0): Print always yields a file immediately; an overlay with labelled buttons then narrows it down to rectangles, a monitor, a window or a saved area, without shortcuts to learn. Predefined targets still capture instantly from their own shortcuts, without any overlay. Success: Print never loses a moment, choosing what to keep takes one gesture, and an area set up once yields an identical, native-resolution PNG on every shortcut press.

## Positioning

Fixed areas bound to a specific monitor, groups that capture several targets from one frame per monitor with one shared timestamp, and true native resolution on mixed-scale setups.

## Operating Context

Runs as a tray icon or Plasma widget. Triggered by the Print key, global shortcuts (registered in KDE's global shortcut system) or the widget. The capture overlay covers every monitor, frozen by default or live (lighter, so a video stays watchable). Areas and groups are created inline in the overlay; the settings window is for rare configuration.

## Capabilities and Constraints

- Targets: areas (one monitor, logical coordinates, auto-named `<connector>-<n>`), built-in per-monitor targets, "All monitors". Groups of targets with their own shortcut.
- No default shortcuts. Cursor off by default, per target and per group.
- Storage: `~/Pictures/glimply/`, flat, `{date}_{time}_{area}.png`, PNG only.
- Feedback: one short tray icon blink after success; a notification with Open and Show in Folder after Print and overlay captures; errors always notify.
- All UI text translatable; English by default, 18 more languages later.
- Not yet: shaped selections (0.3.0), targets that follow a window, an editor window and markup, text recognition, delay timer, other formats, cross-monitor rectangles, interval captures, upload.

## Brand Commitments

"glimply" is a working name; no logo or identity yet. The UI follows the native KDE look and does not introduce its own visual style.

## Product Principles

1. Invisible while capturing: nothing of glimply may appear in a capture (the live overlay hides before it captures).
2. Set up once, then forget: configuration is rare, so it can be explicit; triggering must be effortless.
3. Never swallow errors: skipped targets and failures are always reported, never cropped or dropped silently.
4. Native first: behave like a well-made KDE application (shortcuts, tray, notifications, file manager).
