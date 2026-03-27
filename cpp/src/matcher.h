#pragma once

#include "polymarket.h"
#include "kalshi.h"
#include <string>
#include <vector>
#include <unordered_set>

namespace arb {

struct MatchedPair {
    const PolyMarket* poly;
    const KalshiMarket* kalshi;
    double similarity;
};

struct MatchResult {
    std::vector<MatchedPair> pairs;
    int64_t match_time_ms = 0;
    int poly_count = 0;
    int kalshi_count = 0;
};

// Normalize a market title for comparison
std::string normalize_title(const std::string& title);

// Tokenize a normalized string into a set of tokens
std::unordered_set<std::string> tokenize(const std::string& s);

// Jaccard similarity between two token sets
double jaccard_similarity(const std::unordered_set<std::string>& a,
                          const std::unordered_set<std::string>& b);

// Match polymarket and kalshi markets using fuzzy string matching
// category_filter: if non-empty, only match markets containing this keyword
// min_similarity: minimum Jaccard similarity threshold (0.0-1.0)
// num_threads: number of threads for parallel matching
MatchResult match_markets(const std::vector<PolyMarket>& poly,
                          const std::vector<KalshiMarket>& kalshi,
                          const std::string& category_filter = "",
                          double min_similarity = 0.3,
                          int num_threads = 0);

} // namespace arb
