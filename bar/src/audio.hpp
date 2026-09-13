#pragma once

#include <wp/wp.h>

#include <functional>
#include <string>
#include <vector>

namespace fenriz::bar {

    // Default sink and source, and the devices that could replace them, through WirePlumber.
    class Audio {
    public:
        struct Device {
            guint32 id = 0;
            std::string name; // node.name, what set_default takes
            std::string description;

            bool operator==(const Device&) const = default;
        };

        struct Endpoint {
            const char* media_class;
            guint32 id = 0; // default node, 0 when there is none
            int percent = -1;
            bool muted = false;
            std::vector<Device> devices;
            gint64 set_at = 0; // monotonic time of our last write, see STALE_AFTER_US
        };

        Audio();
        ~Audio();

        Audio(const Audio&) = delete;
        Audio& operator=(const Audio&) = delete;

        // False when there is no PipeWire. The rest arrives asynchronously, through subscribe().
        bool start();
        void subscribe(std::function<void()> listener);

        bool ready() const { return ready_; }
        const Endpoint& sink() const { return sink_; }
        const Endpoint& source() const { return source_; }

        void set_percent(bool source, int percent);
        void set_muted(bool source, bool muted);
        void set_default(bool source, const std::string& node_name);

    private:
        static void on_loaded(WpCore* core, GAsyncResult* res, gpointer data);
        static void on_activated(WpObject* obj, GAsyncResult* res, gpointer data);
        static void on_mixer_changed(WpPlugin* mixer, guint id, gpointer data);
        static void on_changed(gpointer data);

        void refresh();
        void read(Endpoint& ep);
        void write(Endpoint& ep, GVariant* dict);

        WpCore* core_ = nullptr;
        WpPlugin* mixer_ = nullptr;
        WpPlugin* defaults_ = nullptr;
        WpObjectManager* nodes_ = nullptr;
        int pending_ = 0;
        bool ready_ = false;
        Endpoint sink_{"Audio/Sink"};
        Endpoint source_{"Audio/Source"};
        std::vector<std::function<void()>> listeners_;
    };

} // namespace fenriz::bar
