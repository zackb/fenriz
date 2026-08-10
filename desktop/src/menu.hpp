#pragma once

#include <gtk/gtk.h>

#include <string>

#include "config.hpp"

namespace fenriz::desktop::menu {

    // Installs the single "exec" action every menu item drives. Call once per application.
    void install_actions(GtkApplication* app);

    // The context-menu model. Transfers ownership to the caller.
    GMenuModel* build_model(const Config& cfg);

    // The terminal command to offer
    std::string resolve_terminal(const Config& cfg);

    // `fenrizctl exit` when running under fenriz, otherwise a logind session teardown.
    std::string logout_command();

} // namespace fenriz::desktop::menu
