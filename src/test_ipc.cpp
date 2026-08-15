// IPC wire format.

#include "ipc_json.hpp"

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

#include "fenrizctl.hpp"

using namespace fenriz::ipc;

namespace {
    std::string esc(const char* s) {
        std::string out;
        json_escape(out, s);
        return out;
    }
} // namespace

int main() {
    // --- json_escape ------------------------------------------------------------------
    {
        assert(esc(nullptr).empty());
        assert(esc("plain") == "plain");

        // The two structural characters. A title containing either used to be copied straight
        // into the feed and end the JSON string early.
        assert(esc("a\"b") == "a\\\"b");
        assert(esc("a\\b") == "a\\\\b");
        assert(esc("say \"hi\"") == "say \\\"hi\\\"");

        // Whitespace controls are escaped rather than dropped, so a tab in a title survives.
        assert(esc("a\nb") == "a\\nb");
        assert(esc("a\tb") == "a\\tb");
        assert(esc("a\rb") == "a\\rb");
        // Other C0 controls have no meaning in a title and are dropped.
        assert(esc("a\x01\x1f"
                   "b") == "ab");

        // Valid UTF-8 passes through untouched, at every sequence length.
        assert(esc("é") == "é");   // 2-byte
        assert(esc("→") == "→");   // 3-byte
        assert(esc("😀") == "😀"); // 4-byte
        assert(esc("naïve café") == "naïve café");

        // Invalid UTF-8 becomes U+FFFD. This is the one that matters: X11's WM_NAME is Latin-1
        // by convention, so one accented character in a legacy X11 window title is a lone high
        // byte, and JSON strings must be valid UTF-8 — a single such byte used to make the
        // whole NDJSON line unparseable and kill every bar reading the feed.
        const std::string repl = "\xef\xbf\xbd";
        assert(esc("caf\xe9") == "caf" + repl);                       // Latin-1 'é'
        assert(esc("\xff\xfe") == repl + repl);                       // never valid UTF-8
        assert(esc("a\xc3") == "a" + repl);                           // truncated 2-byte sequence
        assert(esc("a\xe2\x82") == "a" + repl + repl);                // truncated 3-byte sequence
        assert(esc("\xc0\xaf") == repl + repl);                       // overlong encoding of '/'
        assert(esc("\xed\xa0\x80") == repl + repl + repl);            // UTF-16 surrogate
        assert(esc("\xf5\x80\x80\x80") == repl + repl + repl + repl); // beyond U+10FFFF

        // Resync: a bad byte must not swallow the valid text after it.
        assert(esc("\xffok") == repl + "ok");

        // Whatever comes out must never contain a bare quote or a bare backslash, or the line
        // it lands in is not JSON at all.
        for (const char* nasty : {"a\"b", "a\\b", "caf\xe9", "\xff\xfe", "\x01\x02"}) {
            const std::string out = esc(nasty);
            for (size_t i = 0; i < out.size(); i++) {
                assert(out[i] != '"');
                if (out[i] == '\\') {
                    assert(i + 1 < out.size());
                    i++; // skip what it escapes
                }
            }
        }
    }

    // --- extract_string ---------------------------------------------------------------
    {
        const std::string line = R"({"cmd":"dispatch","action":"exec","arg":"foot"})";
        assert(extract_string(line, "cmd") == "dispatch");
        assert(extract_string(line, "action") == "exec");
        assert(extract_string(line, "arg") == "foot");
        assert(extract_string(line, "nope").empty());
        assert(extract_string("", "cmd").empty());
        assert(extract_string(R"({"arg":"unterminated)", "arg").empty());

        // Escapes are DECODED, not merely skipped. Taking the character after a backslash
        // verbatim turns the two characters \n into a literal 'n', which silently ran `anb`
        // instead of a two-line command.
        assert(extract_string(R"({"arg":"a\nb"})", "arg") == "a\nb");
        assert(extract_string(R"({"arg":"a\tb"})", "arg") == "a\tb");
        assert(extract_string(R"({"arg":"a\"b"})", "arg") == "a\"b");
        assert(extract_string(R"({"arg":"a\\b"})", "arg") == "a\\b");
        assert(extract_string(R"({"arg":"a\/b"})", "arg") == "a/b");

        // A quote inside the value must not end it early.
        assert(extract_string(R"({"arg":"sh -c \"echo hi\"","x":1})", "arg") == "sh -c \"echo hi\"");
    }

    // --- fenrizctl's encoder round-trips through the compositor's decoder ---------------
    // These two are the only pair on the wire, and they were written independently. Anything
    // the client can encode, the compositor has to get back byte for byte.
    {
        for (const char* s : {"foot",
                              "sh -c 'a,b'",
                              "grim -o \"$M\" - | satty -f -",
                              "a\nb",
                              "a\tb",
                              "back\\slash",
                              "quote\"inside",
                              "swaybg -c #1a1a1a",
                              "both \" and \\ and \n"}) {
            const std::string wire = "{\"arg\":\"" + fenrizctl::escape(s) + "\"}";
            assert(extract_string(wire, "arg") == std::string(s));
        }
    }

    // --- take_lines: framing ------------------------------------------------------------
    {
        auto collect = [](std::string& buf, std::vector<std::string>& out) {
            return take_lines(buf, [&](const std::string& l) { out.push_back(l); });
        };

        // Whole lines in one chunk.
        {
            std::string buf = "one\ntwo\n";
            std::vector<std::string> got;
            assert(collect(buf, got));
            assert(got.size() == 2 && got[0] == "one" && got[1] == "two");
            assert(buf.empty());
        }

        // A line split across two reads. This is the case that silently dropped commands: a
        // stream socket makes no promise that one send arrives as one recv, so a client that
        // writes the JSON and the newline separately lost its first half entirely.
        {
            std::string buf;
            std::vector<std::string> got;
            buf += R"({"cmd":"exit"})";
            assert(collect(buf, got));
            assert(got.empty());  // nothing complete yet...
            assert(!buf.empty()); // ...and the partial line is RETAINED, not discarded
            buf += "\n";
            assert(collect(buf, got));
            assert(got.size() == 1 && got[0] == R"({"cmd":"exit"})");
            assert(buf.empty());
        }

        // Byte-at-a-time delivery still reassembles.
        {
            const std::string msg = R"({"cmd":"reload"})";
            std::string buf;
            std::vector<std::string> got;
            for (char ch : msg) {
                buf += ch;
                assert(collect(buf, got));
            }
            assert(got.empty());
            buf += "\n";
            assert(collect(buf, got));
            assert(got.size() == 1 && got[0] == msg);
        }

        // A command longer than any single read still arrives whole.
        {
            const std::string big = "{\"cmd\":\"dispatch\",\"arg\":\"" + std::string(10000, 'x') + "\"}";
            std::string buf;
            std::vector<std::string> got;
            for (size_t i = 0; i < big.size(); i += 4096) {
                buf += big.substr(i, 4096);
                assert(collect(buf, got));
            }
            buf += "\n";
            assert(collect(buf, got));
            assert(got.size() == 1 && got[0] == big);
        }

        // Trailing partial after a complete line is kept for the next read.
        {
            std::string buf = "done\npart";
            std::vector<std::string> got;
            assert(collect(buf, got));
            assert(got.size() == 1 && got[0] == "done");
            assert(buf == "part");
        }

        // ...but a peer that never sends a newline is refused rather than buffered forever.
        {
            std::string buf(LINE_MAX + 1, 'x');
            std::vector<std::string> got;
            assert(!collect(buf, got));
            assert(got.empty());
        }
    }

    std::printf("ipc wire format: all assertions passed\n");
    return 0;
}
