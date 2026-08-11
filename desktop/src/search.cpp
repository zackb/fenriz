#include "search.hpp"

#include <glib.h>

#include <algorithm>

namespace fenriz::desktop {

    namespace {

        std::string casefold(const std::string& s) {
            char* f = g_utf8_casefold(s.c_str(), -1);
            std::string out = f ? f : std::string();
            g_free(f);
            return out;
        }

        bool is_word_break(char c) { return c == ' ' || c == '\t' || c == '-' || c == '_'; }

        // Does any word of text start with query
        bool any_word_starts_with(const std::string& text, const std::string& query) {
            for (size_t i = 0; i < text.size();) {
                while (i < text.size() && is_word_break(text[i]))
                    i++;
                if (i >= text.size())
                    break;
                if (text.compare(i, query.size(), query) == 0)
                    return true;
                while (i < text.size() && !is_word_break(text[i]))
                    i++;
            }
            return false;
        }

    } // namespace

    int score_match(const std::string& text, const std::string& query) {
        if (text.empty() || query.empty())
            return -1;

        const std::string t = casefold(text);
        const std::string q = casefold(query);

        if (t == q)
            return 1000;
        if (t.compare(0, q.size(), q) == 0)
            return 800;
        if (any_word_starts_with(t, q))
            return 600;
        // A one or two character substring hits nearly everything, so it stays unranked.
        if (g_utf8_strlen(q.c_str(), -1) >= 3 && t.find(q) != std::string::npos)
            return 200;
        return -1;
    }

    int match_score(const MatchFields& f, const std::string& query) {
        int best = score_match(f.name, query);

        for (const std::string& k : f.keywords)
            if (const int s = score_match(k, query); s >= 0)
                best = std::max(best, s - 20);
        if (const int s = score_match(f.generic_name, query); s >= 0)
            best = std::max(best, s - 50);
        if (const int s = score_match(f.comment, query); s >= 0)
            best = std::max(best, s - 100);

        if (!f.exec.empty() && !query.empty() && casefold(f.exec).find(casefold(query)) != std::string::npos)
            best = std::max(best, 180);

        return best;
    }

} // namespace fenriz::desktop
