#include <cassert>
#include <string>
#include <vector>

#include "shot.hpp"

using fenriz::bar::parse_layout;
using fenriz::bar::parse_shot_args;
using fenriz::bar::Rect;
using fenriz::bar::ShotMode;
using fenriz::bar::to_pixels;

int main() {
    // --- arguments ---
    {
        std::string err;
        auto a = parse_shot_args({"region", "--copy", "--edit"}, err);
        assert(a && a->mode == ShotMode::Region && a->copy && a->edit && !a->save);

        a = parse_shot_args({"window", "--save=/tmp/x.png"}, err);
        assert(a && a->mode == ShotMode::Window && a->save && a->path == "/tmp/x.png" && !a->copy);

        a = parse_shot_args({"screen", "--save", "--copy"}, err);
        assert(a && a->save && a->copy && a->path.empty());

        assert(!parse_shot_args({}, err));
        assert(!parse_shot_args({"screen"}, err)); // no destination
        assert(!parse_shot_args({"monitor", "--copy"}, err));
        assert(!parse_shot_args({"screen", "--copy", "--bogus"}, err));
        assert(!parse_shot_args({"screen", "--save="}, err));
    }

    // --- rects ---
    {
        assert((Rect::spanning(10, 20, 4, 2) == Rect{4, 2, 6, 18}));
        assert((Rect{0, 0, 10, 10}.intersect({5, 5, 10, 10}) == Rect{5, 5, 5, 5}));
        assert(Rect{0, 0, 10, 10}.intersect({10, 0, 5, 5}).empty());
    }

    // --- logical to buffer pixels ---
    {
        const Rect out = {1920, 0, 1280, 800}; // second output, scale 2
        assert((to_pixels({2020, 100, 200, 50}, out, 2560, 1600) == Rect{200, 200, 400, 100}));
        // clipped to the output
        assert((to_pixels({1900, -10, 100, 100}, out, 2560, 1600) == Rect{0, 0, 160, 180}));
        assert(to_pixels({0, 0, 100, 100}, out, 2560, 1600).empty());
        // fractional scale 1.25
        assert((to_pixels({0, 0, 1536, 864}, {0, 0, 1536, 864}, 1920, 1080) == Rect{0, 0, 1920, 1080}));
        assert((to_pixels({100, 100, 100, 100}, {0, 0, 1536, 864}, 1920, 1080) == Rect{125, 125, 125, 125}));
    }

    // --- layout ---
    {
        const std::string line =
            R"({"outputs":[{"name":"eDP-1","active":1,"focused":false,"x":0,"y":0,"width":1280,"height":800},)"
            R"({"name":"DP-1","active":3,"focused":true,"x":1280,"y":0,"width":1920,"height":1080}],)"
            R"("windows":[)"
            R"({"workspace":3,"focused":false,"x":1280,"y":0,"width":960,"height":1080},)"
            R"({"workspace":3,"focused":true,"x":2240,"y":0,"width":960,"height":1080},)"
            R"({"workspace":2,"focused":false,"x":1280,"y":0,"width":1920,"height":1080},)"
            R"({"workspace":3,"focused":false,"x":1400,"y":100,"width":300,"height":200}]})";
        auto l = parse_layout(line);
        assert(l && l->outputs.size() == 2 && l->windows.size() == 4);
        assert(l->focused_output()->name == "DP-1");
        assert((l->focused_window()->box == Rect{2240, 0, 960, 1080}));
        assert(l->output_of(l->focused_window()->box)->name == "DP-1");

        const auto& dp = l->outputs[1];
        // the float on top wins, and the hidden workspace 2 window never does
        assert((l->window_at(dp, 1500, 150)->box == Rect{1400, 100, 300, 200}));
        assert((l->window_at(dp, 1300, 900)->box == Rect{1280, 0, 960, 1080}));
        assert(l->window_at(l->outputs[0], 10, 10) == nullptr);

        assert(!parse_layout("not json"));
        assert(!parse_layout(R"({"event":"bell"})"));
    }

    return 0;
}
