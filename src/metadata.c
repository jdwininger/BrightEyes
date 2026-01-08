#include "metadata.h"
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib/gstdio.h>
#include <adwaita.h>

/* Metadata sidebar utilities
 *
 * Helper functions and construction code for the metadata sidebar that
 * shows file details like size, type and properties.
 *
 * Sections: helpers, sidebar construction, update logic.
 */

/* Helper to add a row to a group */
static void
add_pref_row(AdwPreferencesGroup *group, const char *title, const char *subtitle)
{
    GtkWidget *row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    if (subtitle) {
        char *escaped = g_markup_escape_text(subtitle, -1);
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), escaped);
        g_free(escaped);
    }
    adw_preferences_group_add(group, GTK_WIDGET(row));
}

static char *
format_size(guint64 size)
{
    return g_format_size(size);
}

GtkWidget*
find_box_recursive(GtkWidget *parent)
{
    if (GTK_IS_BOX(parent) && g_strcmp0(gtk_widget_get_name(parent), "metadata-content-box") == 0) {
        return parent;
    }

    /* Fallback: If parent is a viewport, its child might be the box */
    if (GTK_IS_VIEWPORT(parent)) {
        GtkWidget *child = gtk_viewport_get_child(GTK_VIEWPORT(parent));
        if (child) return find_box_recursive(child);
    }
    
    /* Fallback: If parent is ScrolledWindow, check child */
    if (GTK_IS_SCROLLED_WINDOW(parent)) {
        GtkWidget *child = gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(parent));
        if (child) return find_box_recursive(child);
    }

    GtkWidget *child = gtk_widget_get_first_child(parent);
    while (child) {
        GtkWidget *found = find_box_recursive(child);
        if (found) return found;
        child = gtk_widget_get_next_sibling(child);
    }
    return NULL;
}

GtkWidget *
metadata_sidebar_new(void)
{
    /* Main container: VBox */
    GtkWidget *container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    GtkWidget *scrolled = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scrolled, TRUE);
    gtk_widget_set_hexpand(scrolled, TRUE); /* Ensure width expansion too */
    
    /* Box to hold groups */
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_name(box, "metadata-content-box");
    
    gtk_widget_set_margin_top(box, 12);
    gtk_widget_set_margin_bottom(box, 12);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);
    gtk_box_set_spacing(GTK_BOX(box), 12);
    
    /* Initial placeholder so we verify it exists */
    GtkWidget *placeholder = gtk_label_new("Select a file...");
    gtk_widget_add_css_class(placeholder, "dim-label");
    gtk_box_append(GTK_BOX(box), placeholder);

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), box);
    
    gtk_box_append(GTK_BOX(container), scrolled);

    /* Bottom Bar with "Open with Editor" button */
    GtkWidget *bottom_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(bottom_bar, "toolbar");
    gtk_widget_set_margin_top(bottom_bar, 6);
    gtk_widget_set_margin_bottom(bottom_bar, 6);
    gtk_widget_set_margin_start(bottom_bar, 6);
    gtk_widget_set_margin_end(bottom_bar, 6);

    GtkWidget *open_btn = gtk_button_new();
    gtk_widget_set_hexpand(open_btn, TRUE);
    gtk_actionable_set_action_name(GTK_ACTIONABLE(open_btn), "win.open-editor");
    
    GtkWidget *btn_content = adw_button_content_new();
    adw_button_content_set_label(ADW_BUTTON_CONTENT(btn_content), "Open in Editor");
    adw_button_content_set_icon_name(ADW_BUTTON_CONTENT(btn_content), "document-edit-symbolic");
    gtk_button_set_child(GTK_BUTTON(open_btn), btn_content);
    
    gtk_box_append(GTK_BOX(bottom_bar), open_btn);
    gtk_box_append(GTK_BOX(container), bottom_bar);

    return container;
}

void
metadata_sidebar_update(GtkWidget *sidebar, const char *path)
{
    // Debug print
    // g_print("DEBUG: metadata_sidebar_update called for path: %s\n", path ? path : "(null)");

    GtkWidget *box = find_box_recursive(sidebar);

    if (!box) {
         g_warning("DEBUG: metadata-content-box NOT FOUND in sidebar widget hierarchy.");
         return;
    }
    
    if (!GTK_IS_BOX(box)) {
        g_warning("DEBUG: Found widget is NOT a box! Type: %s", G_OBJECT_TYPE_NAME(box));
        return;
    }

    /* Clear */
    GtkWidget *item = gtk_widget_get_first_child(box);
    while (item) {
        GtkWidget *next = gtk_widget_get_next_sibling(item);
        gtk_box_remove(GTK_BOX(box), item);
        item = next;
    }

    if (!path) {
        GtkWidget *status = adw_status_page_new();
        adw_status_page_set_icon_name(ADW_STATUS_PAGE(status), "image-missing-symbolic");
        adw_status_page_set_title(ADW_STATUS_PAGE(status), "No Selection");
        gtk_box_append(GTK_BOX(box), status);
        return;
    }

    /* File Details Group */
    GtkWidget *file_group = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(file_group), "File Details");
    gtk_box_append(GTK_BOX(box), file_group);

    GFile *file = g_file_new_for_path(path);
    GError *error = NULL;
    GFileInfo *info = g_file_query_info(file, "standard::*,time::*", G_FILE_QUERY_INFO_NONE, NULL, &error);

    if (info) {
        char *dirname = g_path_get_dirname(path);
        add_pref_row(ADW_PREFERENCES_GROUP(file_group), "Location", dirname);
        g_free(dirname);
        
        add_pref_row(ADW_PREFERENCES_GROUP(file_group), "Name", g_file_info_get_display_name(info));
        
        char *size_str = format_size(g_file_info_get_size(info));
        add_pref_row(ADW_PREFERENCES_GROUP(file_group), "Size", size_str);
        g_free(size_str);

        const char *content_type = g_file_info_get_content_type(info);
        if (content_type) {
             char *desc = g_content_type_get_description(content_type);
             add_pref_row(ADW_PREFERENCES_GROUP(file_group), "Type", desc ? desc : content_type);
             g_free(desc);
        }
        
        GDateTime *cdt = g_file_info_get_creation_date_time(info);
        if (cdt) {
            char *cdate_str = g_date_time_format(cdt, "%Y-%m-%d %H:%M");
            add_pref_row(ADW_PREFERENCES_GROUP(file_group), "Created", cdate_str);
            g_free(cdate_str);
            g_date_time_unref(cdt);
        }

        GDateTime *dt = g_file_info_get_modification_date_time(info);
        if (dt) {
            char *date_str = g_date_time_format(dt, "%Y-%m-%d %H:%M");
            add_pref_row(ADW_PREFERENCES_GROUP(file_group), "Modified", date_str);
            g_free(date_str);
            g_date_time_unref(dt);
        }
        g_object_unref(info);
    } else {
         add_pref_row(ADW_PREFERENCES_GROUP(file_group), "Error", "Could not query file info");
         if (error) {
             add_pref_row(ADW_PREFERENCES_GROUP(file_group), "Message", error->message);
             g_clear_error(&error);
         }
    }
    g_object_unref(file);

    /* Image Properties Group */
    int width, height;
    GdkPixbufFormat *format_info = gdk_pixbuf_get_file_info(path, &width, &height);
    
    if (format_info) {
        GtkWidget *img_group = adw_preferences_group_new();
        adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(img_group), "Image Properties");
        gtk_box_append(GTK_BOX(box), img_group);

        char *dim = g_strdup_printf("%d × %d", width, height);
        add_pref_row(ADW_PREFERENCES_GROUP(img_group), "Dimensions", dim);
        g_free(dim);
        
        gchar *name = gdk_pixbuf_format_get_name(format_info);
        if (name) {
             add_pref_row(ADW_PREFERENCES_GROUP(img_group), "Format", name);
             g_free(name);
        }
    }
}
