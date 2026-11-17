#pragma once

#include <cctype>
#include <string>
#include <vector>

namespace mf {

inline std::string LowerAscii(std::string value) {
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

inline bool IsSearchSpace(unsigned char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

inline std::vector<std::string> SearchTerms(const std::string& query) {
    std::vector<std::string> terms;
    std::string current;
    for (unsigned char ch : query) {
        if (IsSearchSpace(ch)) {
            if (!current.empty()) {
                terms.push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(static_cast<char>(std::tolower(ch)));
    }
    if (!current.empty()) terms.push_back(current);
    return terms;
}

// Removes spaces and ASCII punctuation so "vector-parameter" also matches
// "VectorParameter". Bytes >= 0x80 are kept so Chinese queries stay intact.
inline std::string CompactSearchText(const std::string& value) {
    std::string compact;
    compact.reserve(value.size());
    for (unsigned char ch : value) {
        if (ch >= 0x80) {
            compact.push_back(static_cast<char>(ch));
        } else if (ch > 0x20 && std::isalnum(ch)) {
            compact.push_back(static_cast<char>(std::tolower(ch)));
        }
    }
    return compact;
}

// Every whitespace-separated query term must occur in the haystack. This makes
// "vector parameter", "vector-parameter" and "vectorparam" all find
// VectorParameter.
inline bool MatchesSearch(const std::string& haystack, const std::string& query) {
    const std::vector<std::string> terms = SearchTerms(query);
    if (terms.empty()) return true;

    const std::string lowerHaystack = LowerAscii(haystack);
    const std::string compactHaystack = CompactSearchText(haystack);
    for (const std::string& term : terms) {
        if (lowerHaystack.find(term) != std::string::npos) continue;
        const std::string compactTerm = CompactSearchText(term);
        if (!compactTerm.empty() &&
            compactHaystack.find(compactTerm) != std::string::npos) {
            continue;
        }
        return false;
    }
    return true;
}

} // namespace mf
