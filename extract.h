#pragma once
#include <string>
#include <set>
#include <vector>

// Inspect complete text runs, rather than keeping ASCII fragments of Unicode names.
inline std::set<std::string> extractTags(const std::vector<unsigned char>& bytes) {
    std::set<std::string> tags;
    auto inspect = [&](const std::vector<unsigned>& units) {
        std::string token;
        bool invalid = false, letter = false;
        auto flush = [&] {
            if (!invalid && letter && !token.empty()) tags.insert(token);
            token.clear(); invalid = false; letter = false;
        };
        for (unsigned c : units) {
            bool alpha = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
            bool allowed = alpha || (c >= '0' && c <= '9') || c == '$' || c == '_' || c == '-' || c == '.';
            if (allowed) { token += char(c); letter |= alpha; }
            else if (c >= 128) invalid = true;
            else flush();
        }
        flush();
    };
    // Reserve obvious UTF-16 runs so their low bytes are not indexed as single letters.
    std::vector<bool> wide(bytes.size(), false);
    struct Run { size_t start, end; std::vector<unsigned> units; bool mixed; };
    std::vector<Run> runs;
    for (int endian = 0; endian < 2; ++endian) {
        for (size_t i = 0; i + 3 < bytes.size();) {
            size_t j = i;
            std::vector<unsigned> units;
            while (j + 1 < bytes.size()) {
                unsigned c = endian ? (unsigned(bytes[j]) << 8) | bytes[j+1]
                                    : bytes[j] | (unsigned(bytes[j+1]) << 8);
                if (c < 32 || c > 126) break;
                units.push_back(c); j += 2;
            }
            if (units.size() >= 3) {
                // A non-ASCII adjacent UTF-16 unit means this may be a mixed-script name.
                auto unit = [&](size_t p) -> unsigned {
                    return endian ? (unsigned(bytes[p]) << 8) | bytes[p+1]
                                  : bytes[p] | (unsigned(bytes[p+1]) << 8);
                };
                bool mixed = (j + 1 < bytes.size() && unit(j) >= 128)
                          || (i >= 2 && unit(i-2) >= 128);
                runs.push_back({i, j, std::move(units), mixed});
                for (size_t k = i; k < j; ++k) wide[k] = true;
                i = j;
            } else ++i;
        }
    }
    // Opposite-endian, one-byte-shifted interpretations must not turn a
    // rejected mixed-script run back into an accepted ASCII-only fragment.
    std::vector<bool> rejected(bytes.size(), false);
    for (const auto& run : runs) if (run.mixed)
        for (size_t k = run.start; k < run.end; ++k) rejected[k] = true;
    for (const auto& run : runs) {
        bool reject = false;
        for (size_t k = run.start; k < run.end; ++k) reject |= rejected[k];
        if (!reject) inspect(run.units);
    }
    std::vector<unsigned> units;
    units.reserve(bytes.size());
    for (size_t i = 0; i < bytes.size(); ++i) units.push_back(wide[i] ? 0 : bytes[i]);
    inspect(units);
    return tags;
}
