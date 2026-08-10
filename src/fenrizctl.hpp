#pragma once

#include <glob.h>

#include <cstdlib>
#include <string>
#include <vector>

namespace fenrizctl {

    // Where to reach the compositor. Args are the environment.
    inline std::string socket_path(const char* env_socket, const char* xdg, const char* display) {
        if (env_socket && *env_socket)
            return env_socket;
        if (!xdg || !*xdg)
            return "";
        if (display && *display)
            return std::string(xdg) + "/fenriz-" + display + ".sock";

        glob_t g = {};
        std::string found;
        if (glob((std::string(xdg) + "/fenriz-*.sock").c_str(), 0, nullptr, &g) == 0 && g.gl_pathc == 1)
            found = g.gl_pathv[0];
        globfree(&g);
        return found;
    }

    inline std::string socket_path() {
        return socket_path(getenv("FENRIZ_SOCKET"), getenv("XDG_RUNTIME_DIR"), getenv("WAYLAND_DISPLAY"));
    }

    enum class Mode {
        None,
        State,
        Watch,
        Send,
    };

    struct Command {
        Mode mode = Mode::None;
        std::string json;
        std::string output;
        std::string error;
    };

    inline std::string escape(const std::string& s) {
        std::string out;
        for (unsigned char c : s) {
            if (c == '"' || c == '\\')
                out += '\\', out += (char)c;
            else if (c == '\n')
                out += "\\n";
            else if (c >= 0x20)
                out += (char)c;
        }
        return out;
    }

    inline bool is_action(const std::string& s) {
        static const char* actions[] = {"exec",
                                        "killactive",
                                        "exit",
                                        "focusnext",
                                        "focusprev",
                                        "focusleft",
                                        "focusright",
                                        "focusup",
                                        "focusdown",
                                        "togglelayout",
                                        "fullscreen",
                                        "togglefloating",
                                        "workspace",
                                        "movetoworkspace",
                                        "pin"};
        for (const char* a : actions)
            if (s == a)
                return true;
        return false;
    }

    // join argv[i..] with spaces
    inline std::string join(const std::vector<std::string>& a, size_t i) {
        std::string s;
        for (; i < a.size(); ++i) {
            if (!s.empty())
                s += ' ';
            s += a[i];
        }
        return s;
    }

    inline Command usage_error(const std::string& msg) { return {Mode::None, "", "", msg}; }

    inline Command send(const std::string& json, const std::string& output = "") {
        return {Mode::Send, json + "\n", output, ""};
    }

    // args excludes argv[0]
    inline Command parse(const std::vector<std::string>& args) {
        if (args.empty())
            return usage_error("missing command");
        const std::string& cmd = args[0];
        const size_t n = args.size();

        if (cmd == "state")
            return {Mode::State, "", "", ""};
        if (cmd == "watch")
            return {Mode::Watch, "", "", ""};

        if (cmd == "unlock" || cmd == "reload")
            return send("{\"cmd\":\"" + cmd + "\"}");
        if (cmd == "exit")
            return send("{\"cmd\":\"exit\"}");

        if (cmd == "workspace") {
            if (n != 2)
                return usage_error("workspace takes a number 1-10");
            int ws = std::atoi(args[1].c_str());
            if (ws < 1 || ws > 10)
                return usage_error("workspace must be 1-10, got '" + args[1] + "'");
            return send("{\"cmd\":\"workspace\",\"n\":" + std::to_string(ws) + "}");
        }

        if (cmd == "dpms") {
            if (n < 2 || n > 3 || (args[1] != "on" && args[1] != "off"))
                return usage_error("usage: fenrizctl dpms on|off [OUTPUT]");
            std::string j = "{\"cmd\":\"dpms\",\"on\":" + std::string(args[1] == "on" ? "true" : "false");
            if (n == 3)
                j += ",\"name\":\"" + escape(args[2]) + "\"";
            return send(j + "}", n == 3 ? args[2] : "");
        }

        if (cmd == "output") {
            if (n != 3 || (args[2] != "on" && args[2] != "off"))
                return usage_error("usage: fenrizctl output NAME on|off");
            bool on = args[2] == "on";
            std::string j = "{\"cmd\":\"output\",\"name\":\"" + escape(args[1]) +
                            "\",\"enabled\":" + std::string(on ? "true" : "false") + "}";
            return send(j, on ? "" : args[1]);
        }

        if (cmd == "lid") {
            if (n != 2 || (args[1] != "open" && args[1] != "closed"))
                return usage_error("usage: fenrizctl lid open|closed");
            return send("{\"cmd\":\"lid\",\"closed\":" + std::string(args[1] == "closed" ? "true" : "false") + "}");
        }

        size_t i = (cmd == "dispatch") ? 1 : 0;
        if (i >= n)
            return usage_error("dispatch needs an action");
        const std::string& action = args[i];
        if (!is_action(action))
            return usage_error("unknown command or action: '" + action + "'");

        std::string j = "{\"cmd\":\"dispatch\",\"action\":\"" + action + "\"";
        std::string arg = join(args, i + 1);
        if (!arg.empty())
            j += ",\"arg\":\"" + escape(arg) + "\"";
        else if (action == "exec" || action == "workspace" || action == "movetoworkspace")
            return usage_error(action + " needs an argument");
        return send(j + "}");
    }

} // namespace fenrizctl
