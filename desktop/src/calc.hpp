#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace fenriz::desktop::calc {

    // Evaluates + - * / % ^, parentheses, unary signs and decimals. nullopt on a syntax
    // error, division by zero, or a non-finite result.
    std::optional<std::string> evaluate(std::string_view expr);

    // Launcher entry point. "=expr" always evaluates, otherwise only a query made purely of
    // arithmetic with at least one binary operator.
    std::optional<std::string> detect(std::string_view query);

} // namespace fenriz::desktop::calc
