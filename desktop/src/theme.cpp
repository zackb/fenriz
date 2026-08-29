#include "theme.hpp"

#include <string>

namespace fenriz::desktop::theme {

    namespace {

        // grab whatever we need for the sheet from the config
        std::string prelude(const Config& cfg) {
            const std::string a = std::to_string(cfg.shell_opacity);
            return "@define-color fenriz_accent " + cfg.accent + ";" + "@define-color fenriz_accent2 " +
                   cfg.accent_gradient + ";" + "@define-color fenriz_fill_popover alpha(@popover_bg_color," + a + ");" +
                   "@define-color fenriz_fill_base alpha(@theme_base_color," + a + ");" +
                   "@define-color fenriz_fill_window alpha(@window_bg_color," + a + ");" +
                   ".lock-wallpaper { filter: blur(" + std::to_string(cfg.lock_blur) + "px); }";
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

window.fenriz-osd { background: transparent; }

/* duration MUST match FADE_MS in osd.cpp */
.fenriz-osd .osd-pill { opacity: 1; transition: opacity 250ms ease-out; }
.fenriz-osd .osd-pill.fading { opacity: 0; }

.fenriz-osd .osd-pill {
  padding: 14px 20px;
  border-radius: 999px;
  background-color: @fenriz_fill_popover;
}
.fenriz-osd levelbar trough {
  min-height: 6px;
  border: none;
  border-radius: 3px;
  background-color: alpha(currentColor, 0.2);
}
.fenriz-osd levelbar block.filled {
  border: none;
  border-radius: 3px;
  background-image: linear-gradient(135deg, @fenriz_accent, @fenriz_accent2);
}

window.fenriz-notify { background: transparent; }

/* duration MUST match FADE_MS in toast.cpp */
.fenriz-notify .fenriz-toast { opacity: 1; transition: opacity 200ms ease-out; }
.fenriz-notify .fenriz-toast.fading { opacity: 0; }

.fenriz-notify .fenriz-toast {
  padding: 14px 16px;
  border-radius: 12px;
  background-color: @fenriz_fill_base;
}
.fenriz-notify .fenriz-toast.critical { border: 1px solid @fenriz_error; }
.fenriz-notify .toast-summary { font-weight: bold; }
.fenriz-notify .toast-body { color: alpha(currentColor, 0.8); }
.fenriz-notify .toast-icon { border-radius: 6px; }
.fenriz-notify .toast-action { padding: 4px 12px; border-radius: 8px; }

.fenriz-notify-history { padding: 12px; }
.fenriz-notify-history .history-header { padding: 2px 4px 10px 4px; }
.fenriz-notify-history .history-title { font-weight: bold; font-size: 1.05em; }
.fenriz-notify-history .history-clear { padding: 2px 12px; border-radius: 8px; }
.fenriz-notify-history .history-empty {
  padding: 20px;
  color: alpha(currentColor, 0.55);
}
/* rows carry no spacing of their own, so the gap has to come from the card */
.fenriz-notify-history list > row { padding: 3px 0; }
/* A tint of the foreground, not @fenriz_fill_base: many themes (Catppuccin) define
   theme_base_color and window_bg_color identically, which leaves the row invisible
   against the panel. This lifts it in a light theme too. */
.fenriz-notify-history .history-item {
  padding: 10px 12px;
  border-radius: 10px;
  background-color: alpha(currentColor, 0.07);
}
.fenriz-notify-history list > row:hover .history-item {
  background-color: alpha(currentColor, 0.12);
}
.fenriz-notify-history .history-item.critical { border: 1px solid @fenriz_error; }
.fenriz-notify-history .history-summary { font-weight: bold; }
.fenriz-notify-history .history-body { color: alpha(currentColor, 0.8); }
.fenriz-notify-history .history-footer { font-size: 0.85em; color: alpha(currentColor, 0.55); }
.fenriz-notify-history .history-icon { border-radius: 6px; }
/* the dismiss button surfaces on hover, so a quiet list stays quiet */
.fenriz-notify-history .history-dismiss {
  min-width: 24px;
  min-height: 24px;
  padding: 0;
  border-radius: 12px;
  opacity: 0;
  transition: opacity 150ms ease-out;
}
.fenriz-notify-history list > row:hover .history-dismiss { opacity: 1; }

.fenriz-background popover.menu > contents {
  min-width: 200px;
  border-radius: 12px;
  background-color: @fenriz_fill_popover;
}

window.fenriz-shell { background: transparent; }

.fenriz-launcher, .fenriz-wallpaper, .fenriz-polkit, .fenriz-notify-history {
  border-radius: 12px;
  background-color: @fenriz_fill_window;
}

.fenriz-polkit { padding: 20px 24px; }
.fenriz-wallpaper.no-search { padding-top: 10px; }

.fenriz-launcher list, .fenriz-launcher list > row,
.fenriz-launcher scrolledwindow, .fenriz-wallpaper scrolledwindow,
.fenriz-notify-history list, .fenriz-notify-history list > row,
.fenriz-notify-history scrolledwindow {
  background: transparent;
}

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

    std::string sheet(const Config& cfg) { return prelude(cfg) + SHEET; }

    void install(const Config& cfg) {
        const std::string css = sheet(cfg);
        GtkCssProvider* provider = gtk_css_provider_new();
        gtk_css_provider_load_from_string(provider, css.c_str());
        gtk_style_context_add_provider_for_display(
            gdk_display_get_default(), GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_USER + 1);
        g_object_unref(provider);
    }

} // namespace fenriz::desktop::theme
