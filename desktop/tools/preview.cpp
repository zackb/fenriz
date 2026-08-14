// Render one of the shell's styled surfaces to a PNG, without restarting the session.
//
// It installs the real sheet (theme::install with your own config, so the accents and
// shell_opacity are the ones you actually run), maps a widget carrying the CSS class you name,
// and writes what GTK drew. The card sits on a colourful backdrop so translucency and the
// blur-facing parts of the style are visible.
//
// The widget has to be mapped for GTK to render it, so this needs a compositor. Point it at a
// throwaway one rather than your session — `make preview` does that for you.
//
//   preview fenriz-launcher /tmp/out.png
//   preview osd-pill /tmp/pill.png --in fenriz-osd --content pill --size 340x110
//
// It also prints the card's natural size, which is what you need when a style depends on it.

#include <gtk/gtk.h>

#include <string>

#include "config.hpp"
#include "theme.hpp"

namespace {

    struct Options {
        std::string css_class;
        std::string out;
        std::string content = "plain";
        std::string in; // ancestor class, for the sheet's descendant selectors
        int width = 480, height = 320; // a request; the compositor and GTK have the last word
    };

    Options opts;
    GtkWidget* card;
    GtkWidget* frame;
    GdkPaintable* paintable;
    GMainLoop* loop;

    // The backdrop only. The surfaces themselves come from the real sheet.
    // The inset is padding here rather than a margin on the card, so the natural size the
    // tool reports is the card's own and not the card plus its margins.
    constexpr const char* BACKDROP = ".preview-backdrop {"
                                     "  padding: 24px;"
                                     "  background-image: linear-gradient(60deg,#6fd6e8,#f0a25a 60%,#2b2438);"
                                     "}";

    void on_css_error(GtkCssProvider*, GtkCssSection* section, GError* error, gpointer) {
        char* where = gtk_css_section_to_string(section);
        g_printerr("css: %s: %s\n", where, error->message);
        g_free(where);
    }

    gboolean shoot(gpointer) {
        // GTK settles the layout over a few passes once the compositor has sized the window;
        // rendering before it lands crops the card.
        static int tries = 0;
        if (gdk_paintable_get_intrinsic_height(paintable) < opts.height && ++tries < 20)
            return G_SOURCE_CONTINUE;

        int min = 0, nat = 0;
        gtk_widget_measure(card, GTK_ORIENTATION_VERTICAL, gtk_widget_get_width(card), &min, &nat, nullptr, nullptr);
        g_print("%s: card %dx%d, natural height %d (min %d)\n",
                opts.css_class.c_str(),
                gtk_widget_get_width(card),
                gtk_widget_get_height(card),
                nat,
                min);

        // The paintable's intrinsic size is the widget's own. Asking for anything else
        // rescales the render, and a 1px ring disappears into subpixels.
        const int w = gdk_paintable_get_intrinsic_width(paintable);
        const int h = gdk_paintable_get_intrinsic_height(paintable);
        GtkSnapshot* snapshot = gtk_snapshot_new();
        gdk_paintable_snapshot(paintable, snapshot, w, h);
        GskRenderNode* node = gtk_snapshot_free_to_node(snapshot);
        if (!node) {
            g_printerr("nothing was drawn — is '%s' a class the sheet styles?\n", opts.css_class.c_str());
            exit(1);
        }

        GskRenderer* renderer = gsk_gl_renderer_new();
        GError* error = nullptr;
        if (!gsk_renderer_realize_for_display(renderer, gdk_display_get_default(), &error)) {
            g_printerr("renderer: %s\n", error->message);
            exit(1);
        }
        GdkTexture* texture = gsk_renderer_render_texture(renderer, node, nullptr);
        if (!gdk_texture_save_to_png(texture, opts.out.c_str())) {
            g_printerr("could not write %s\n", opts.out.c_str());
            exit(1);
        }
        g_print("wrote %s (%dx%d)\n", opts.out.c_str(), w, h);
        g_main_loop_quit(loop);
        return G_SOURCE_REMOVE;
    }

    // The pill can be filled with the same children osd.cpp gives it, so its reported natural
    // height is the real one.
    void add_pill_content(GtkWidget* box) {
        GtkWidget* image = gtk_image_new_from_icon_name("audio-volume-high-symbolic");
        gtk_image_set_pixel_size(GTK_IMAGE(image), 24);
        gtk_box_append(GTK_BOX(box), image);
        GtkWidget* bar = gtk_level_bar_new_for_interval(0, 100);
        gtk_level_bar_set_mode(GTK_LEVEL_BAR(bar), GTK_LEVEL_BAR_MODE_CONTINUOUS);
        gtk_level_bar_set_value(GTK_LEVEL_BAR(bar), 65);
        gtk_widget_set_hexpand(bar, TRUE);
        gtk_widget_set_valign(bar, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(box), bar);
    }

    bool parse_args(int argc, char** argv) {
        for (int i = 1; i < argc; i++) {
            const std::string arg = argv[i];
            if (arg == "--content" && i + 1 < argc)
                opts.content = argv[++i];
            else if (arg == "--in" && i + 1 < argc)
                opts.in = argv[++i];
            else if (arg == "--size" && i + 1 < argc)
                sscanf(argv[++i], "%dx%d", &opts.width, &opts.height);
            else if (opts.css_class.empty())
                opts.css_class = arg;
            else if (opts.out.empty())
                opts.out = arg;
        }
        return !opts.css_class.empty() && !opts.out.empty();
    }

} // namespace

int main(int argc, char** argv) {
    if (!parse_args(argc, argv)) {
        g_printerr("usage: preview CSS-CLASS OUT.png [--in ANCESTOR-CLASS] "
                   "[--content plain|pill] [--size WxH]\n");
        return 2;
    }
    gtk_init();

    fenriz::desktop::theme::install(fenriz::desktop::Config::load());
    GtkCssProvider* backdrop = gtk_css_provider_new();
    g_signal_connect(backdrop, "parsing-error", G_CALLBACK(on_css_error), nullptr);
    gtk_css_provider_load_from_string(backdrop, BACKDROP);
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(), GTK_STYLE_PROVIDER(backdrop), GTK_STYLE_PROVIDER_PRIORITY_USER + 2);

    frame = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(frame, "preview-backdrop");
    // Most of the sheet's cards are matched through an ancestor (".fenriz-osd .osd-pill"), so
    // without this they come out unstyled.
    if (!opts.in.empty())
        gtk_widget_add_css_class(frame, opts.in.c_str());
    gtk_widget_set_size_request(frame, opts.width, opts.height);
    gtk_widget_set_halign(frame, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(frame, GTK_ALIGN_CENTER);

    card = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    gtk_widget_add_css_class(card, opts.css_class.c_str());
    gtk_widget_set_hexpand(card, TRUE);
    if (opts.content == "pill") {
        add_pill_content(card);
        gtk_widget_set_valign(card, GTK_ALIGN_CENTER); // let it take its natural height
    } else {
        gtk_widget_set_vexpand(card, TRUE);
    }
    gtk_box_append(GTK_BOX(frame), card);

    // A tiling compositor will size the window itself; the frame's size request is what keeps
    // the rendered area the size that was asked for.
    GtkWidget* window = gtk_window_new();
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
    gtk_window_set_child(GTK_WINDOW(window), frame);
    paintable = gtk_widget_paintable_new(frame);
    gtk_window_present(GTK_WINDOW(window));

    loop = g_main_loop_new(nullptr, FALSE);
    g_timeout_add(500, shoot, nullptr); // one frame is not enough; let it settle
    g_main_loop_run(loop);
    return 0;
}
