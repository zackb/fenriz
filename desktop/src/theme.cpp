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

        std::string px(double v) {
            std::string s = std::to_string(v);
            s.erase(s.find_last_not_of('0') + 1);
            if (s.back() == '.')
                s.pop_back();
            return s + "px";
        }

        // A gradient ring around a card the compositor blurs behind.
        std::string ring(const std::string& selector,
                         int radius,
                         const std::string& fill,
                         const std::string& c0,
                         const std::string& c1) {
            const std::string mid = "mix(" + c0 + "," + c1 + ",0.5)";
            const std::string r = px(radius);
            // Wide enough that the arc is solid across the ring after antialiasing; the
            // border-box clip and the padding box trim what spills either side.
            const std::string stops[4] = {px(radius - 2.5), px(radius - 1.5), px(radius + 0.5), px(radius + 1.5)};
            auto arc = [&](const char* at, const std::string& c) {
                return "radial-gradient(circle " + r + " at " + at + ", transparent " + stops[0] + ", " + c + " " +
                       stops[1] + ", " + c + " " + stops[2] + ", transparent " + stops[3] + ")";
            };
            return selector +
                   "{"
                   "border:1px solid transparent;"
                   "border-radius:" +
                   r +
                   ";"
                   "background-origin:border-box;"
                   "background-repeat:no-repeat;"
                   "background-image:" +
                   arc("100% 100%", c0) + "," + // top-left
                   arc("0% 100%", mid) + "," +  // top-right
                   arc("100% 0%", mid) + "," +  // bottom-left
                   arc("0% 0%", c1) + "," +     // bottom-right
                   "linear-gradient(90deg," + c0 + "," + mid + ")," + "linear-gradient(90deg," + mid + "," + c1 + ")," +
                   "linear-gradient(180deg," + c0 + "," + mid + ")," + "linear-gradient(180deg," + mid + "," + c1 +
                   ")," + "linear-gradient(" + fill + "," + fill +
                   ");"
                   "background-size:" +
                   r + " " + r + "," + r + " " + r + "," + r + " " + r + "," + r + " " + r +
                   ",100% 1px,100% 1px,1px 100%,1px 100%,auto;"
                   "background-position:top left,top right,bottom left,bottom right,top,bottom,left,right,center;"
                   "background-clip:border-box,border-box,border-box,border-box,"
                   "border-box,border-box,border-box,border-box,padding-box;"
                   "}";
        }

        // Every card the compositor blurs behind.
        std::string rings() {
            const char* acc = "@fenriz_accent";
            const char* acc2 = "@fenriz_accent2";
            const char* err = "@fenriz_error";
            return ring(".fenriz-osd .osd-pill", PILL_RADIUS, "@fenriz_fill_popover", acc, acc2) +
                   ring(".fenriz-notify .fenriz-toast", CARD_RADIUS, "@fenriz_fill_base", acc, acc2) +
                   ring(".fenriz-notify .fenriz-toast.critical", CARD_RADIUS, "@fenriz_fill_base", err, err) +
                   ring(".fenriz-background popover.menu > contents", CARD_RADIUS, "@fenriz_fill_popover", acc, acc2) +
                   ring(".fenriz-launcher,.fenriz-wallpaper,.fenriz-polkit",
                        CARD_RADIUS,
                        "@fenriz_fill_window",
                        acc,
                        acc2);
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

.fenriz-osd .osd-pill { padding: 14px 20px; }
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

.fenriz-notify .fenriz-toast { padding: 14px 16px; }
.fenriz-notify .toast-summary { font-weight: bold; }
.fenriz-notify .toast-body { color: alpha(currentColor, 0.8); }
.fenriz-notify .toast-icon { border-radius: 6px; }
.fenriz-notify .toast-action { padding: 4px 12px; border-radius: 8px; }

.fenriz-background popover.menu > contents { min-width: 200px; }

window.fenriz-shell { background: transparent; }

.fenriz-polkit { padding: 20px 24px; }
.fenriz-wallpaper.no-search { padding-top: 10px; }

.fenriz-launcher list, .fenriz-launcher list > row,
.fenriz-launcher scrolledwindow, .fenriz-wallpaper scrolledwindow {
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

    std::string sheet(const Config& cfg) { return prelude(cfg) + SHEET + rings(); }

    void install(const Config& cfg) {
        const std::string css = sheet(cfg);
        GtkCssProvider* provider = gtk_css_provider_new();
        gtk_css_provider_load_from_string(provider, css.c_str());
        gtk_style_context_add_provider_for_display(
            gdk_display_get_default(), GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_USER + 1);
        g_object_unref(provider);
    }

} // namespace fenriz::desktop::theme
