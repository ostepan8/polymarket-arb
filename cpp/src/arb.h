#pragma once

#include "matcher.h"
#include <vector>
#include <string>

namespace arb {

// Fee constants
constexpr double POLY_FEE = 0.01;    // 1%
constexpr double KALSHI_FEE = 0.038;  // 3.8%

enum class ArbDirection {
    NONE,
    POLY_YES_KALSHI_NO,   // Buy YES on Poly, buy NO on Kalshi
    POLY_NO_KALSHI_YES,   // Buy NO on Poly, buy YES on Kalshi
};

struct ArbOpportunity {
    const PolyMarket* poly;
    const KalshiMarket* kalshi;
    double similarity;
    ArbDirection direction;
    double poly_price;    // price we'd pay on Polymarket
    double kalshi_price;  // price we'd pay on Kalshi
    double arb_pct;       // arbitrage percentage (after fees)
    double raw_arb_pct;   // arb before fees
    // For $100 bet sizing
    double profit_per_100;
};

struct ArbResult {
    std::vector<ArbOpportunity> opportunities;
    int total_pairs_checked = 0;
    int64_t calc_time_ms = 0;
};

// Calculate arb opportunities from matched pairs
ArbResult calculate_arbs(const std::vector<MatchedPair>& pairs, double min_arb_pct = 0.0);

// Helper: direction to string
std::string direction_str(ArbDirection dir);

} // namespace arb
