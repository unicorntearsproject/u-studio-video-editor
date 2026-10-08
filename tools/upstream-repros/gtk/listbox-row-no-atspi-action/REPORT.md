**Title:** Activatable GtkListBoxRow exposes no AT-SPI action, so assistive technology can't activate it

## Steps to reproduce

1. Build and run the example below: a `GtkListBox` with one activatable `GtkListBoxRow` (`row-activated` prints), next to a `GtkExpander` and a `GtkButton` for comparison.

   ```sh
   cc listrow.c -o listrow $(pkg-config --cflags --libs gtk4)
   ```
2. Inspect it with Accerciser, or list each object's actions over AT-SPI (the probe script linked below does that on a private Xvfb and AT-SPI bus).

<details>
<summary>listrow.c</summary>

```c
#include <gtk/gtk.h>

static void on_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer data)
{
    g_print("row-activated\n");
}

static void on_activate(GtkApplication *app, gpointer data)
{
    GtkWidget *window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "listrow");
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);

    GtkWidget *list = gtk_list_box_new();
    GtkWidget *row = gtk_list_box_row_new();
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), gtk_label_new("Activatable row"));
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
    gtk_list_box_append(GTK_LIST_BOX(list), row);
    g_signal_connect(list, "row-activated", G_CALLBACK(on_row_activated), NULL);
    gtk_box_append(GTK_BOX(box), list);

    GtkWidget *expander = gtk_expander_new("GtkExpander");
    gtk_expander_set_child(GTK_EXPANDER(expander), gtk_label_new("inside"));
    gtk_box_append(GTK_BOX(box), expander);
    gtk_box_append(GTK_BOX(box), gtk_button_new_with_label("GtkButton"));

    gtk_window_set_child(GTK_WINDOW(window), box);
    gtk_window_present(GTK_WINDOW(window));
}

int main(int argc, char **argv)
{
    GtkApplication *app = gtk_application_new("org.example.ListRow", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
```
</details>

## Current behavior

```
[list item] name='' actions=[]
[button] name='GtkExpander' actions=['activate'] expandable
[button] name='GtkButton' actions=['click']
```

The row has no Action interface entries, so an AT-SPI client can't emit `row-activated`, which a click, Enter or Space all do. This also affects libadwaita's `AdwActionRow` (activatable) and `AdwExpanderRow`, whose header is an activatable `GtkListBoxRow`: its accessible reports `expandable`, but nothing can expand it. `gtk_atspi_get_action_vtable()` (`gtk/a11y/gtkatspiaction.c`) special-cases buttons, entries, `GtkExpander`, switches and colour swatches; a row falls through to the generic widget vtable, which only lists actions installed on the widget itself.

(A smaller point: the row's name is empty although its only child is a label.)

## Expected outcome

An activatable row exposes an action (for example `activate`) that emits `row-activated`, as `GtkExpander` exposes `activate` and `GtkButton` `click`. #8354 (sortable column headers) was the same kind of gap.

## Version information

- GTK 4.22.4 (Fedora 44 package); the code involved looks unchanged on main (17d3508)
- Fedora 44, x86_64; Mesa 26.1.4
- X11 (Xvfb, with a private AT-SPI bus); not tried on Wayland

## Additional information

Found while scripting UI tests over AT-SPI for a GTK4/libadwaita app: expander rows in a preferences dialog could only be opened by clicking at screen coordinates. Repro and probe: https://github.com/unicorntearsproject/u-studio-video-editor/tree/main/tools/upstream-repros/gtk/listbox-row-no-atspi-action
