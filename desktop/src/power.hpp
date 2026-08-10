#pragma once

#include <memory>
#include <vector>

struct zwlr_output_power_manager_v1;

namespace fenriz::desktop {

    struct OutputPowerControl; // defined in power.cpp

    // wlr-output-power-management-unstable-v1 client: blanks the screens on idle.
    class OutputPower {
    public:
        OutputPower();
        ~OutputPower();

        OutputPower(const OutputPower&) = delete;
        OutputPower& operator=(const OutputPower&) = delete;

        // False when the compositor does not implement the protocol
        bool start();
        bool available() const { return manager_ != nullptr; }

        void set_all(bool on);

    private:
        zwlr_output_power_manager_v1* manager_ = nullptr;
        std::vector<std::unique_ptr<OutputPowerControl>> controls_;
    };

} // namespace fenriz::desktop
