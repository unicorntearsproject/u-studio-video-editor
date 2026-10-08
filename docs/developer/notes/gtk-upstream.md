# GTK and libadwaita upstream reports

[Docs home](../../README.md) › [Developer docs](../README.md) › [Implementation notes](README.md) › GTK and libadwaita upstream reports

GTK and libadwaita behaviour we work around that looks like a bug
upstream: what it is, what we do instead, and its upstream report. Each
report is written, ready to paste into GNOME's GitLab, as `REPORT.md`
next to its C repro in
[`tools/upstream-repros/`](../../../tools/upstream-repros/README.md). The
AT-SPI ones run on a private Xvfb and AT-SPI bus with
`tools/upstream-repros/atspi-harness.sh`. Checked against GTK 4.22.4 and
libadwaita 1.9.2 (Fedora 44) and read against main on 2026-09-28.

## Reported

| Behaviour | Our workaround | Upstream |
|---|---|---|
| An activatable `GtkListBoxRow` exposes no AT-SPI action, so nothing can activate it over AT-SPI. That covers `AdwActionRow` and `AdwExpanderRow`: Help's sections open only by a click at screen coordinates. | The demo tour clicks the header's position | GTK: [report](../../../tools/upstream-repros/gtk/listbox-row-no-atspi-action/REPORT.md), waiting to be filed |
| `GtkPopoverMenu` items (from a `GMenu`) have an empty accessible name; their `labelled-by` target is empty too. Their `click` action works. | None: AT-SPI test scripts can't find a menu item by its name | GTK: [report](../../../tools/upstream-repros/gtk/popover-menu-item-no-name/REPORT.md), waiting to be filed |
| `AdwSpinRow` isn't in the AT-SPI tree at all: its group's list skips it, so a screen reader or AT-SPI script can't find or change it (U-Stu Titles' Speed, position and size rows). | U-Stu Titles' inspector builds its number rows as an `AdwActionRow` with a `GtkSpinButton` suffix labelled with the row's title | libadwaita: [report](../../../tools/upstream-repros/libadwaita/spin-row-no-atspi/REPORT.md), waiting to be filed |
| `AdwOverlaySplitView`, collapsed with its sidebar shown, lays a shield over the content that takes every drop. | The inspector docks beside the content at 1280 px and wider ([App shell](app-shell.md)) | libadwaita: [report](../../../tools/upstream-repros/libadwaita/overlay-split-view-shield-drop/REPORT.md), waiting to be filed |

The owner files these from their GNOME GitLab account (there's no GitLab
login on the dev machine). Replace "waiting to be filed" with the issue
link once each is filed.

## Not bugs

| Behaviour | Why |
|---|---|
| `GtkFileDialog` starts in the process's working directory when given no folder ([Titles](titles.md)). | GTK's own chooser does this on purpose: the `startup-mode` setting picks Recent or the cwd, and `gtkfilechooserwidget.c` explains the cwd case for apps started from a terminal |
| A window with `AdwBreakpoint`s no longer takes its minimum size from its content ([App shell](app-shell.md)). | Documented: `AdwWindow` and `AdwBreakpointBin` need an explicit minimum size when breakpoints are used |
| Application accelerators run before the focused widget's key handlers; a `GtkListView` binds about 200 rows when it fills; a modal `AdwDialog` dims the window ([M4 findings](../../audit/2026-09-25-m4-mlt-findings.md)). | Design choices in GTK and libadwaita |
| Fedora's libadwaita pulls libappstream and libcurl. | Distribution packaging |
| `g_main_context_invoke()` runs the function inline when the calling thread owns or can acquire the context. | Documented GLib behaviour |
