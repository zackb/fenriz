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

.cleaning-scrim { background-color: rgba(0,0,0,0.88); }
.cleaning-label { font-size: 26px; color: white; }
.cleaning-time  { font-size: 76px; font-weight: 300; color: white; font-feature-settings: "tnum"; }
.cleaning-hint  { font-size: 16px; color: alpha(white, 0.75); }

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

.fenriz-launcher, .fenriz-wallpaper, .fenriz-polkit, .fenriz-notify-history, .fenriz-cleaning {
  border-radius: 12px;
  background-color: @fenriz_fill_window;
}

.fenriz-polkit, .fenriz-cleaning { padding: 20px 24px; }
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

window.fenriz-bar, window.fenriz-island { background: transparent; }

/* MUST match PILL_HEIGHT in bar/src/island.cpp */
.fenriz-bar .bar-capsule {
  min-height: 30px;
  padding: 3px;
  border-radius: 999px;
  background-color: @fenriz_fill_window;
}
.fenriz-bar .bar-window { padding: 0 14px; }
.fenriz-bar .bar-title { font-weight: 500; }
.fenriz-bar .workspace {
  min-width: 24px;
  min-height: 24px;
  padding: 0;
  border: none;
  border-radius: 999px;
  box-shadow: none;
  background: transparent;
  font-weight: 600;
  font-feature-settings: "tnum";
  color: alpha(currentColor, 0.55);
  transition: background-color 150ms, color 150ms;
}
.fenriz-bar .workspace:hover { background-color: alpha(currentColor, 0.1); color: currentColor; }
.fenriz-bar .workspace.visible { color: currentColor; }
.fenriz-bar .workspace.active {
  color: white;
  background-image: linear-gradient(135deg, @fenriz_accent, @fenriz_accent2);
}
.fenriz-bar .workspace.urgent { color: @fenriz_error; }

/* the shape is clipped and animated in code; it only supplies the fill */
.fenriz-island .island-shape { background-color: @fenriz_fill_window; }
.fenriz-island .island-clock { font-weight: 600; font-feature-settings: "tnum"; }
.fenriz-island .island-page { padding: 18px 20px 20px 20px; min-width: 320px; }
.fenriz-island .island-header,
.fenriz-island .island-back {
  padding: 4px 10px;
  border: none;
  border-radius: 12px;
  box-shadow: none;
  background: transparent;
}
.fenriz-island .island-header:hover,
.fenriz-island .island-back:hover { background-color: alpha(currentColor, 0.08); }
.fenriz-island .island-time { font-size: 44px; font-weight: 300; font-feature-settings: "tnum"; }
.fenriz-island .island-date { color: alpha(currentColor, 0.7); }
.fenriz-island calendar { border: none; background: transparent; }
.fenriz-island calendar > grid > label.day-number:selected {
  color: white;
  background-image: linear-gradient(135deg, @fenriz_accent, @fenriz_accent2);
}

.fenriz-bar button.bar-capsule {
  padding: 0 12px;
  border: none;
  box-shadow: none;
  outline: none;
}
.fenriz-bar button.bar-capsule:hover { background-color: alpha(@fenriz_fill_window, 1); }
.fenriz-bar .bar-glyph { padding: 0 10px; }

.fenriz-island .island-title { font-weight: 600; }
.fenriz-island .island-section {
  margin-top: 6px;
  font-size: 0.85em;
  font-weight: 600;
  color: alpha(currentColor, 0.6);
}
.fenriz-island .island-controls { margin-top: 8px; }
.fenriz-island .island-icon-button,
.fenriz-island .island-flat,
.fenriz-island .island-player {
  min-width: 32px;
  min-height: 32px;
  padding: 0 6px;
  border: none;
  border-radius: 10px;
  box-shadow: none;
  background: transparent;
}
.fenriz-island .island-icon-button:hover,
.fenriz-island .island-flat:hover,
.fenriz-island .island-player:hover { background-color: alpha(currentColor, 0.08); }
.fenriz-island .island-player:checked { background-color: alpha(currentColor, 0.14); }
.fenriz-island .island-row-icon { min-width: 44px; } /* a 32px icon button plus its padding */

.fenriz-island scale.island-slider trough { min-height: 6px; border-radius: 3px; background-color: alpha(currentColor, 0.15); }
.fenriz-island scale.island-slider highlight {
  border-radius: 3px;
  background-image: linear-gradient(90deg, @fenriz_accent, @fenriz_accent2);
}
.fenriz-island scale.island-slider slider { min-width: 14px; min-height: 14px; margin: -5px; }

.fenriz-island .island-device { padding: 4px 6px; border-radius: 8px; }
.fenriz-island .island-device:hover { background-color: alpha(currentColor, 0.06); }

.fenriz-island levelbar.island-level { min-width: 120px; }
.fenriz-island levelbar.island-level trough { min-height: 4px; border: none; border-radius: 2px; background-color: alpha(currentColor, 0.2); }
.fenriz-island levelbar.island-level block.filled {
  border: none;
  border-radius: 2px;
  background-image: linear-gradient(90deg, @fenriz_accent, @fenriz_accent2);
}
.fenriz-island .island-art { border-radius: 5px; }
.fenriz-island .island-media { font-weight: 600; }

.fenriz-island .island-media-card { padding: 8px; border-radius: 14px; background-color: alpha(currentColor, 0.06); }
.fenriz-island .island-cover-small { border-radius: 8px; }
.fenriz-island .island-cover { margin: 6px 0; border-radius: 16px; }
.fenriz-island .island-track-title { font-weight: 600; }
.fenriz-island .island-track-artist { color: alpha(currentColor, 0.7); }
.fenriz-island .island-time-small { font-size: 0.85em; font-feature-settings: "tnum"; color: alpha(currentColor, 0.7); }
.fenriz-island .island-media-button {
  min-width: 44px;
  min-height: 44px;
  border: none;
  border-radius: 999px;
  box-shadow: none;
  background: transparent;
}
.fenriz-island .island-media-button:hover { background-color: alpha(currentColor, 0.1); }

.fenriz-bar .bar-battery { font-feature-settings: "tnum"; }

.fenriz-island .island-tiles { margin-top: 12px; }
.fenriz-island .island-tile {
  padding: 10px 6px;
  border: none;
  border-radius: 14px;
  box-shadow: none;
  background-color: alpha(currentColor, 0.07);
}
.fenriz-island .island-tile:hover { background-color: alpha(currentColor, 0.12); }
.fenriz-island .island-tile:checked {
  color: white;
  background-image: linear-gradient(135deg, @fenriz_accent, @fenriz_accent2);
}
.fenriz-island .island-tile-label { font-size: 0.85em; font-weight: 600; }

.fenriz-island .island-footer {
  margin-top: 10px;
  padding-top: 8px;
  border-top: 1px solid alpha(currentColor, 0.1);
}
.fenriz-island .island-footer-text { font-size: 0.85em; font-feature-settings: "tnum"; color: alpha(currentColor, 0.75); }

.fenriz-island .island-battery { font-size: 1.4em; font-weight: 300; font-feature-settings: "tnum"; }
.fenriz-island .island-profile { padding: 6px 10px; box-shadow: none; }
.fenriz-island .island-profile:checked {
  color: white;
  background-image: linear-gradient(135deg, @fenriz_accent, @fenriz_accent2);
}
.fenriz-island .island-actions { margin-top: 6px; }
.fenriz-island .island-action {
  padding: 8px 10px;
  border: none;
  border-radius: 10px;
  box-shadow: none;
  background: transparent;
}
.fenriz-island .island-action:hover { background-color: alpha(currentColor, 0.08); }
.fenriz-island .island-action.confirm { color: white; background-color: @fenriz_error; }

.fenriz-island .island-stats { margin-top: 4px; }
.fenriz-island .island-stat { font-weight: 600; font-feature-settings: "tnum"; }
.fenriz-island .island-sparkline { color: @fenriz_accent; margin-bottom: 6px; }
.fenriz-island .alert { color: @fenriz_error; }

/* a tile whose body toggles and whose arrow opens a page */
.fenriz-island .island-split-tile > .island-tile { border-top-right-radius: 0; border-bottom-right-radius: 0; }
.fenriz-island .island-tile-more {
  min-width: 24px;
  padding: 0 4px;
  border: none;
  border-radius: 0 14px 14px 0;
  box-shadow: none;
  background-color: alpha(currentColor, 0.1);
}
.fenriz-island .island-tile-more:hover { background-color: alpha(currentColor, 0.16); }
.fenriz-island .island-empty { padding: 16px; color: alpha(currentColor, 0.55); }

.fenriz-island .island-device-row { border-radius: 10px; }
.fenriz-island .island-device-row:hover { background-color: alpha(currentColor, 0.06); }
.fenriz-island .island-device-button {
  padding: 6px 8px;
  border: none;
  box-shadow: none;
  background: transparent;
}
.fenriz-island .island-device-row.connected image { color: @fenriz_accent; }
.fenriz-island .island-forget {
  min-width: 32px;
  min-height: 32px;
  padding: 0;
  border: none;
  border-radius: 8px;
  box-shadow: none;
  background: transparent;
  opacity: 0;
  transition: opacity 150ms ease-out;
}
.fenriz-island .island-device-row:hover .island-forget { opacity: 0.7; }
.fenriz-island .island-forget:hover { opacity: 1; background-color: alpha(@fenriz_error, 0.2); }
.fenriz-island .island-dim { color: alpha(currentColor, 0.5); }
.fenriz-island .island-password { padding: 2px 8px 8px 38px; }
.fenriz-island .island-password .fenriz-field { --fenriz-fill: alpha(currentColor, 0.06); padding: 0 8px; min-height: 34px; }
.fenriz-island .island-join {
  padding: 0 14px;
  border: none;
  border-radius: 10px;
  box-shadow: none;
  color: white;
  background-image: linear-gradient(135deg, @fenriz_accent, @fenriz_accent2);
}
.fenriz-island scrolledwindow { background: transparent; }

.fenriz-bar .bar-tray { padding: 0 8px; }
.fenriz-bar .bar-tray-item { padding: 0 4px; min-height: 24px; border-radius: 8px; }
.fenriz-bar .bar-tray-item:hover { background-color: alpha(currentColor, 0.1); }
.fenriz-bar .bar-tray-item.attention { background-color: alpha(@fenriz_error, 0.3); }
.fenriz-bar popover.menu > contents { min-width: 180px; border-radius: 12px; background-color: @fenriz_fill_popover; }
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
