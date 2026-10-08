**Title:** AdwOverlaySplitView: with the sidebar overlaid, the shield swallows drag-and-drop onto the content

## Steps to reproduce

1. Build and run the example below: an `AdwOverlaySplitView` whose sidebar holds a label with a `GtkDragSource`, and whose content holds a label with a `GtkDropTarget` (a drop prints).

   ```sh
   cc shield.c -o shield $(pkg-config --cflags --libs libadwaita-1)
   ./shield overlay    # collapsed, sidebar shown over the content
   ./shield docked     # not collapsed, sidebar beside the content (control)
   ```
2. Drag "Drag me" from the sidebar onto "Drop here" in the content.

<details>
<summary>shield.c</summary>

```c
#include <adwaita.h>

static const char *mode = "overlay";

static GdkContentProvider *on_prepare(GtkDragSource *source, double x, double y, gpointer data)
{
    return gdk_content_provider_new_typed(G_TYPE_STRING, "dragged text");
}

static gboolean on_drop(GtkDropTarget *target, const GValue *value, double x, double y, gpointer data)
{
    g_print("dropped: %s\n", g_value_get_string(value));
    return TRUE;
}

static void on_activate(GtkApplication *app, gpointer data)
{
    GtkWidget *window = adw_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "shield repro");
    gtk_window_set_default_size(GTK_WINDOW(window), 900, 500);

    GtkWidget *source = gtk_label_new("Drag me");
    gtk_widget_set_size_request(source, 200, 200);
    GtkDragSource *drag = gtk_drag_source_new();
    g_signal_connect(drag, "prepare", G_CALLBACK(on_prepare), NULL);
    gtk_widget_add_controller(source, GTK_EVENT_CONTROLLER(drag));

    GtkWidget *target = gtk_label_new("Drop here");
    gtk_widget_set_hexpand(target, TRUE);
    gtk_widget_set_vexpand(target, TRUE);
    GtkDropTarget *drop = gtk_drop_target_new(G_TYPE_STRING, GDK_ACTION_COPY);
    g_signal_connect(drop, "drop", G_CALLBACK(on_drop), NULL);
    gtk_widget_add_controller(target, GTK_EVENT_CONTROLLER(drop));

    GtkWidget *split = adw_overlay_split_view_new();
    adw_overlay_split_view_set_sidebar(ADW_OVERLAY_SPLIT_VIEW(split), source);
    adw_overlay_split_view_set_content(ADW_OVERLAY_SPLIT_VIEW(split), target);
    // "overlay": collapsed, sidebar shown over the content (the shield is up).
    // "docked":  not collapsed, sidebar beside the content (control).
    adw_overlay_split_view_set_collapsed(ADW_OVERLAY_SPLIT_VIEW(split), g_str_equal(mode, "overlay"));
    adw_overlay_split_view_set_show_sidebar(ADW_OVERLAY_SPLIT_VIEW(split), TRUE);

    adw_application_window_set_content(ADW_APPLICATION_WINDOW(window), split);
    gtk_window_present(GTK_WINDOW(window));
}

int main(int argc, char **argv)
{
    if (argc > 1)
        mode = argv[1];
    AdwApplication *app = adw_application_new("org.example.ShieldRepro", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    int status = g_application_run(G_APPLICATION(app), 1, argv);
    g_object_unref(app);
    return status;
}
```
</details>

## Current behavior

`docked`: "dropped: dragged text". `overlay`: nothing; the drop never reaches the content's `GtkDropTarget`.

While the view is collapsed and the sidebar shown, the shield (`update_shield()` in `adw-overlay-split-view.c`, there so that a click outside closes the sidebar) is picked for every pointer event over the content, drag-and-drop included. A drop target in the content can't receive a drop, whatever its `can-target`.

## Expected outcome

Dragging from the overlaid sidebar onto the content drops there, as it does when the sidebar is docked: for example, by letting drag-and-drop through the shield, or by hiding the sidebar when a drag enters the content. Dragging items from a sidebar (a media browser, an effects list) onto a canvas is a common layout for this widget.

## Version information

- libadwaita 1.9.2, GTK 4.22.4 (Fedora 44 packages); `adw-overlay-split-view.c` on main (0ffcd2c) has the same shield
- Fedora 44, x86_64; Mesa 26.1.4
- X11 (Xvfb), driven with slow synthetic XTest drags (the `docked` control drops with the same drag); not tried by hand on Wayland

## Additional information

Repro and the drag script: https://github.com/unicorntearsproject/u-studio-video-editor/tree/main/tools/upstream-repros/libadwaita/overlay-split-view-shield-drop
