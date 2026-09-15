#include "calc.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace fenriz::desktop::calc {

    namespace {

        // Recursive descent, each parse_* returns NAN on error, which propagates.
        struct Parser {
            std::string_view s;
            size_t pos = 0;

            char peek() {
                while (pos < s.size() && s[pos] == ' ')
                    pos++;
                return pos < s.size() ? s[pos] : '\0';
            }

            // expr := term (('+' | '-') term)*
            double parse_expr() {
                double v = parse_term();
                for (char c = peek(); c == '+' || c == '-'; c = peek()) {
                    pos++;
                    const double r = parse_term();
                    v = c == '+' ? v + r : v - r;
                }
                return v;
            }

            // term := unary (('*' | '/' | '%') unary)*
            double parse_term() {
                double v = parse_unary();
                for (char c = peek(); c == '*' || c == '/' || c == '%'; c = peek()) {
                    pos++;
                    const double r = parse_unary();
                    if ((c == '/' || c == '%') && r == 0)
                        return NAN;
                    v = c == '*' ? v * r : c == '/' ? v / r : std::fmod(v, r);
                }
                return v;
            }

            // unary := ('-' | '+') unary | power; binds looser than ^, so -2^2 is -4.
            double parse_unary() {
                const char c = peek();
                if (c == '-' || c == '+') {
                    pos++;
                    const double v = parse_unary();
                    return c == '-' ? -v : v;
                }
                return parse_power();
            }

            // power := primary ('^' unary)?, right associative.
            double parse_power() {
                const double base = parse_primary();
                if (peek() != '^')
                    return base;
                pos++;
                return std::pow(base, parse_unary());
            }

            // primary := number | '(' expr ')'
            double parse_primary() {
                if (peek() == '(') {
                    pos++;
                    const double v = parse_expr();
                    if (peek() != ')')
                        return NAN;
                    pos++;
                    return v;
                }
                const size_t start = pos;
                while (pos < s.size() && ((s[pos] >= '0' && s[pos] <= '9') || s[pos] == '.'))
                    pos++;
                if (pos == start)
                    return NAN;
                const std::string num(s.substr(start, pos - start));
                char* end = nullptr;
                const double v = std::strtod(num.c_str(), &end);
                return *end == '\0' ? v : NAN; // rejects "1.2.3"
            }
        };

        bool is_digit(char c) { return c >= '0' && c <= '9'; }

    } // namespace

    std::optional<std::string> evaluate(std::string_view expr) {
        Parser p{expr};
        double v = p.parse_expr();
        if (p.peek() != '\0' || !std::isfinite(v))
            return std::nullopt;
        if (v == 0)
            v = 0; // no "-0"
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.12g", v);
        return std::string(buf);
    }

    std::optional<std::string> detect(std::string_view query) {
        if (!query.empty() && query.front() == '=')
            return evaluate(query.substr(1));

        // A binary operator is an operator following an operand, which rules out "42" and "-5".
        bool has_binary_op = false;
        bool after_operand = false;
        for (char c : query) {
            if (is_digit(c) || c == '.' || c == ')') {
                after_operand = true;
            } else if (c == '+' || c == '-' || c == '*' || c == '/' || c == '%' || c == '^') {
                has_binary_op |= after_operand;
                after_operand = false;
            } else if (c == '(') {
                after_operand = false;
            } else if (c != ' ') {
                return std::nullopt;
            }
        }
        return has_binary_op ? evaluate(query) : std::nullopt;
    }

} // namespace fenriz::desktop::calc
