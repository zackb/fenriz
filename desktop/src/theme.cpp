#include "theme.hpp"

#include <string>

namespace fenriz::desktop::theme {

    namespace {

        // grab whatever we need for the sheet from the config
        std::string prelude(const Config& cfg) {
            return "@define-color fenriz_accent " + cfg.accent + ";" + "@define-color fenriz_accent2 " +
                   cfg.accent_gradient + ";" + ".lock-wallpaper { filter: blur(" + std::to_string(cfg.lock_blur) +
                   "px); }";
        }

        constexpr const char* SHEET = R"css(
@define-color fenriz_error #f38ba8;

.fenriz-background { background: transparent; }

.fenriz-field {
  --fenriz-fill: @view_bg_color;
  border: 2px solid transparent;
  border-radius: 12px;
  background-color: transparent;
  background-origin: border-box;
  background-clip: padding-box, border-box;
  background-image: linear-gradient(var(--fenriz-fill), var(--fenriz-fill)),
                    linear-gradient(135deg, alpha(currentColor, 0.2), alpha(currentColor, 0.2));
  box-shadow: none;
  outline: none;
  transition: background-image 150ms;
}

.fenriz-field:focus-within,
.fenriz-wallpaper .fenriz-field {
  background-image: linear-gradient(var(--fenriz-fill), var(--fenriz-fill)),
                    linear-gradient(135deg, @fenriz_accent, @fenriz_accent2);
}

.fenriz-field.error {
  background-image: linear-gradient(var(--fenriz-fill), var(--fenriz-fill)),
                    linear-gradient(135deg, @fenriz_error, @fenriz_error);
}

.lock-scrim { background-color: rgba(0,0,0,0.45); }
.lock-clock { font-size: 76px; font-weight: 300; color: white; }
.lock-date  { font-size: 18px; color: alpha(white, 0.85); }
/* The fill has to be opaque or the ring gradient bleeds through the interior. */
.lock-entry { --fenriz-fill: #1e1e2e; color: white; }
.lock-entry, .lock-entry text {
  caret-color: transparent;
  -gtk-secondary-caret-color: transparent;
}
.lock-error { font-size: 14px; color: #ff8080; }
.lock-error.status { color: alpha(white, 0.85); }

.fenriz-wallpaper flowbox > flowboxchild,
.fenriz-wallpaper flowbox > flowboxchild:selected {
  background-color: transparent;
  background-image: none;
  outline: none;
  box-shadow: none;
}
.fenriz-wallpaper .wallpaper-tile { padding: 4px; border-radius: 7px; }
.fenriz-wallpaper .wallpaper-tile picture { border-radius: 3px; }
.fenriz-wallpaper flowbox > flowboxchild:selected .wallpaper-tile {
  background-image: linear-gradient(135deg, @fenriz_accent, @fenriz_accent2);
}
)css";

    } // namespace

    void install(const Config& cfg) {
        const std::string css = prelude(cfg) + SHEET;
        GtkCssProvider* provider = gtk_css_provider_new();
        gtk_css_provider_load_from_string(provider, css.c_str());
        gtk_style_context_add_provider_for_display(
            gdk_display_get_default(), GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_USER + 1);
        g_object_unref(provider);
    }

} // namespace fenriz::desktop::theme
