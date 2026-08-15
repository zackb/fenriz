#pragma once

// IPC wire format: escaping values out, pulling fields back in, and
// splitting a byte stream into lines. Separated for unit testing.

#include <cstddef>
#include <string>

namespace fenriz::ipc {

    // Longest command line accepted from a client.
    constexpr size_t LINE_MAX = 64 * 1024;

    // Append `s` to `out` escaped as the body of a JSON string (no surrounding quotes).
    inline void json_escape(std::string& out, const char* s) {
        if (!s)
            return;
        const auto* p = reinterpret_cast<const unsigned char*>(s);
        while (*p) {
            const unsigned char c = *p;
            if (c == '"' || c == '\\') {
                out += '\\';
                out += (char)c;
                p++;
            } else if (c == '\n') {
                out += "\\n";
                p++;
            } else if (c == '\t') {
                out += "\\t";
                p++;
            } else if (c == '\r') {
                out += "\\r";
                p++;
            } else if (c < 0x20) {
                p++; // other C0 controls have no place in a title; drop them
            } else if (c < 0x80) {
                out += (char)c;
                p++;
            } else {
                // Multi-byte, validate the whole sequence before copying any of it.
                int len = 0;
                unsigned char lo = 0x80, hi = 0xBF;
                if (c >= 0xC2 && c <= 0xDF)
                    len = 2;
                else if (c >= 0xE0 && c <= 0xEF) {
                    len = 3;
                    if (c == 0xE0)
                        lo = 0xA0;
                    else if (c == 0xED)
                        hi = 0x9F; // no surrogates
                } else if (c >= 0xF0 && c <= 0xF4) {
                    len = 4;
                    if (c == 0xF0)
                        lo = 0x90;
                    else if (c == 0xF4)
                        hi = 0x8F; // no > U+10FFFF
                }
                bool ok = len > 0;
                for (int i = 1; ok && i < len; i++) {
                    const unsigned char b = p[i];
                    const unsigned char blo = (i == 1) ? lo : 0x80;
                    const unsigned char bhi = (i == 1) ? hi : 0xBF;
                    ok = b >= blo && b <= bhi;
                }
                if (ok) {
                    out.append(reinterpret_cast<const char*>(p), (size_t)len);
                    p += len;
                } else {
                    out += "\xef\xbf\xbd"; // U+FFFD
                    p++;                   // resync one byte at a time
                }
            }
        }
    }

    // Decode one JSON escape sequence starting at `line[p]` (which is the backslash) into
    // `out`, returning the index just past it.
    inline size_t json_unescape_one(const std::string& line, size_t p, std::string& out) {
        if (p + 1 >= line.size()) {
            out += line[p];
            return p + 1;
        }
        switch (line[p + 1]) {
        case 'n':
            out += '\n';
            break;
        case 't':
            out += '\t';
            break;
        case 'r':
            out += '\r';
            break;
        case 'b':
            out += '\b';
            break;
        case 'f':
            out += '\f';
            break;
        // \" \\ \/ and anything unrecognized stand for the character itself.
        default:
            out += line[p + 1];
            break;
        }
        return p + 2;
    }

    // Pull a "key":"value" string out of a command line, decoding escapes.
    inline std::string extract_string(const std::string& line, const char* key) {
        const std::string pat = std::string("\"") + key + "\":\"";
        const size_t p0 = line.find(pat);
        if (p0 == std::string::npos)
            return "";
        std::string out;
        for (size_t p = p0 + pat.size(); p < line.size();) {
            if (line[p] == '"')
                return out;
            if (line[p] == '\\')
                p = json_unescape_one(line, p, out);
            else
                out += line[p++];
        }
        return ""; // unterminated
    }

    // Split complete lines out of an accumulating per-client buffer, calling `fn` for each and
    // erasing what it consumed. Returns false when the buffer has grown past LINE_MAX without a
    // newline, which means the peer is not speaking the protocol and should be dropped.
    template <typename Fn> inline bool take_lines(std::string& buf, Fn&& fn) {
        size_t start = 0;
        for (size_t nl; (nl = buf.find('\n', start)) != std::string::npos; start = nl + 1)
            fn(buf.substr(start, nl - start));
        buf.erase(0, start);
        return buf.size() <= LINE_MAX;
    }

} // namespace fenriz::ipc
