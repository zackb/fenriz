#pragma once

#include <string>
#include <vector>

namespace fenriz::desktop {

    // Searchable text of one desktop entry.
    struct MatchFields {
        std::string name;
        std::string generic_name;
        std::string comment;
        std::vector<std::string> keywords;
        std::string exec;
    };

    // Relevance of one field: 1000 exact, 800 whole-string prefix, 600 word prefix,
    // 200 substring, queries of 3+ characters only, -1 no match.
    int score_match(const std::string& text, const std::string& query);

    // Best score across an entry's fields, -1 when nothing matches. Higher is better.
    int match_score(const MatchFields& f, const std::string& query);

} // namespace fenriz::desktop
