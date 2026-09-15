#include <cassert>

#include "calc.hpp"

using namespace fenriz::desktop;

namespace {

    bool eq(std::string_view expr, const char* want) {
        const auto got = calc::evaluate(expr);
        return got && *got == want;
    }

    void test_arithmetic() {
        assert(eq("1+2*3", "7"));
        assert(eq("(1+2)*3", "9"));
        assert(eq(" 10 - 4 - 3 ", "3"));
        assert(eq("-2^2", "-4"));
        assert(eq("2^3^2", "512"));
        assert(eq("2^-1", "0.5"));
        assert(eq("7 % 3", "1"));
        assert(eq("--3", "3"));
        assert(eq("0.1+0.2", "0.3"));
        assert(eq("2/3", "0.666666666667"));
        assert(eq("-0*1", "0"));
        assert(eq(".5*2", "1"));
    }

    void test_rejects() {
        assert(!calc::evaluate(""));
        assert(!calc::evaluate("1/0"));
        assert(!calc::evaluate("1%0"));
        assert(!calc::evaluate("2+"));
        assert(!calc::evaluate("(1+2"));
        assert(!calc::evaluate("1+2)"));
        assert(!calc::evaluate("1.2.3"));
        assert(!calc::evaluate("."));
        assert(!calc::evaluate("10^1000"));
    }

    void test_detect() {
        assert(calc::detect("=2*3") == "6");
        assert(calc::detect("=42") == "42");
        assert(!calc::detect("="));
        assert(!calc::detect("=2+"));
        assert(calc::detect("2 + 2") == "4");
        assert(calc::detect("(1+2)*3") == "9");
        assert(calc::detect("-5+1") == "-4");

        // App-like queries never auto-trigger.
        assert(!calc::detect("7zip"));
        assert(!calc::detect("2048"));
        assert(!calc::detect("42"));
        assert(!calc::detect("-5"));
        assert(!calc::detect("(5)"));
        assert(!calc::detect("k3b"));
        assert(!calc::detect("1+"));
        assert(!calc::detect(""));
    }

} // namespace

int main() {
    test_arithmetic();
    test_rejects();
    test_detect();
    return 0;
}
