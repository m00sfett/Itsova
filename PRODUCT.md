# Product

<!-- impeccable:product-schema 1 -->

## Platform

desktop (KDE Plasma on Wayland, native Qt 6 Widgets with the Breeze style; not one of web/ios/android)

## Users

First the author, a KDE Plasma power user with two monitors at different scales. Later other KDE Plasma users who find the project on GitHub. Their job: capture the same fixed screen regions, single monitors or all monitors again and again, at the exact moment a shortcut is pressed.

## Product Purpose

glimply captures predefined targets instantly: no selection, no dialog, no overlay, no confirmation. It exists because general screenshot tools forget the last region, are not bound to a monitor and ask for input. Success: a region is set up once and every shortcut press yields an identical, native-resolution PNG in a predictable place.

## Positioning

Fixed regions bound to a specific monitor, groups that capture several targets from one frame per monitor with one shared timestamp, and true native resolution on mixed-scale setups.

## Operating Context

Runs as a tray icon only. Triggered by global shortcuts (registered in KDE's global shortcut system) or from the tray menu. Configuration happens rarely, in one settings window opened from the tray. Defining a region uses a selection overlay on the chosen monitor plus numeric fields.

## Capabilities and Constraints

- Targets: regions (one monitor, logical coordinates, auto-named `<connector>-<n>`), built-in per-monitor targets, "All monitors". Groups of targets with their own shortcut.
- No default shortcuts. Cursor off by default, per target and per group.
- Storage: `~/Pictures/glimply/`, flat, `{date}_{time}_{region}.png`, PNG only.
- Feedback: one short tray icon blink after success; notifications only for errors.
- All UI text translatable; English by default, 18 more languages later.
- Not in v0.1: interval captures, cross-monitor regions, clipboard, editing, upload.

## Brand Commitments

"glimply" is a working name; no logo or identity yet. The UI follows the native KDE look and does not introduce its own visual style.

## Product Principles

1. Invisible while capturing: nothing may appear on screen during a capture.
2. Set up once, then forget: configuration is rare, so it can be explicit; triggering must be effortless.
3. Never swallow errors: skipped targets and failures are always reported, never cropped or dropped silently.
4. Native first: behave like a well-made KDE application (shortcuts, tray, notifications, file manager).
