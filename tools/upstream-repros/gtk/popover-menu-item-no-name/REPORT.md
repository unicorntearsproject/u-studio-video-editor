**Title:** Popover menu items (GtkPopoverMenu from a GMenu) have an empty accessible name

## Steps to reproduce

1. Build and run the example below: a `GtkMenuButton` whose menu model has two items, "First item" and "Second item".

   ```sh
   cc menuitem.c -o menuitem $(pkg-config --cflags --libs gtk4)
   ```
2. Open the menu, and inspect the menu items with Accerciser, or over AT-SPI (the probe script linked below opens the menu through the button's `click` action and prints each item).

<details>
<summary>menuitem.c</summary>

```c
#include <gtk/gtk.h>

static void on_item(GSimpleAction *action, GVariant *parameter, gpointer data)
{
    g_print("activated %s\n", g_action_get_name(G_ACTION(action)));
}

static void on_activate(GtkApplication *app, gpointer data)
{
    static const GActionEntry entries[] = {{"first", on_item}, {"second", on_item}};
    g_action_map_add_action_entries(G_ACTION_MAP(app), entries, G_N_ELEMENTS(entries), NULL);
    GMenu *menu = g_menu_new();
    g_menu_append(menu, "First item", "app.first");
    g_menu_append(menu, "Second item", "app.second");

    GtkWidget *window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "menuitem");
    GtkWidget *button = gtk_menu_button_new();
    gtk_menu_button_set_label(GTK_MENU_BUTTON(button), "Menu");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(button), G_MENU_MODEL(menu));
    gtk_window_set_child(GTK_WINDOW(window), button);
    gtk_window_set_default_size(GTK_WINDOW(window), 400, 300);
    gtk_window_present(GTK_WINDOW(window));
    g_object_unref(menu);
}

int main(int argc, char **argv)
{
    GtkApplication *app = gtk_application_new("org.example.MenuItem", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
```
</details>

## Current behavior

```
[menu item] name='' actions=['click'] labelled-by=['']
    child [panel] name='' actions=[]
[menu item] name='' actions=['click'] labelled-by=['']
    child [panel] name='' actions=[]
```

Both items are in the tree and their `click` action works (it activates `app.first`), but their accessible name is empty. `GtkModelButton` sets `GTK_ACCESSIBLE_RELATION_LABELLED_BY` to its label (`update_accessible_properties()` in `gtk/gtkmodelbutton.c`), yet the relation's target has an empty name too, and no label with the item's text appears in the item's subtree. A screen reader, or anything looking an item up by name, has nothing to go on.

## Expected outcome

Each menu item's accessible name is its label: "First item", "Second item".

## Version information

- GTK 4.22.4 (Fedora 44 package); the code involved looks unchanged on main (17d3508)
- Fedora 44, x86_64; Mesa 26.1.4
- X11 (Xvfb, with a private AT-SPI bus); not tried on Wayland

## Additional information

Found while scripting UI tests over AT-SPI for a GTK4 app: menu items could only be told apart by their order. Repro and probe: https://github.com/unicorntearsproject/u-studio-video-editor/tree/main/tools/upstream-repros/gtk/popover-menu-item-no-name
