#pragma once

#include <gtk/gtk.h>

#include <string>

namespace fenriz::bar {

    // Shows a freedesktop icon name or an absolute file path (SNI items and desktop entries use paths).
    // Returns false, leaving the image untouched, when neither resolves.
    inline bool set_image_icon(GtkImage* image, const std::string& icon) {
        if (icon.starts_with('/')) {
            if (!g_file_test(icon.c_str(), G_FILE_TEST_IS_REGULAR))
                return false;
            gtk_image_set_from_file(image, icon.c_str());
            return true;
        }
        GtkIconTheme* theme = gtk_icon_theme_get_for_display(gdk_display_get_default());
        if (icon.empty() || !gtk_icon_theme_has_icon(theme, icon.c_str()))
            return false;
        gtk_image_set_from_icon_name(image, icon.c_str());
        return true;
    }

} // namespace fenriz::bar
