#include "arb.h"
#include <algorithm>
#include <cmath>
#include <chrono>

namespace arb {

std::string direction_str(ArbDirection dir) {
    switch (dir) {
        case ArbDirection::POLY_YES_KALSHI_NO:
            return "Poly YES / Kalshi NO";
        case ArbDirection::POLY_NO_KALSHI_YES:
            return "Poly NO / Kalshi YES";
        default:
            return "None";
    }
}

ArbResult calculate_arbs(const std::vector<MatchedPair>& pairs, double min_arb_pct) {
    ArbResult result;
    auto start = std::chrono::steady_clock::now();

    result.total_pairs_checked = static_cast<int>(pairs.size());

    for (auto& pair : pairs) {
        const auto* poly = pair.poly;
        const auto* kalshi = pair.kalshi;

        // Direction 1: Buy YES on Polymarket, Buy NO on Kalshi
        // We pay poly_yes on Poly and kalshi_no on Kalshi
        // Total cost = poly_yes + kalshi_no_ask
        // If everything resolves, one side pays $1
        // Profit = 1.0 - poly_yes - kalshi_no_price - fees
        {
            double poly_cost = poly->yes_price;
            // For Kalshi NO, we want the ask price (what we'd buy at)
            double kalshi_no_cost = kalshi->no_price;

            if (poly_cost > 0.0 && kalshi_no_cost > 0.0) {
                double total_cost = poly_cost + kalshi_no_cost;
                double raw_arb = (1.0 - total_cost) * 100.0;  // percentage
                double fee_cost = poly_cost * POLY_FEE + kalshi_no_cost * KALSHI_FEE;
                double net_arb = (1.0 - total_cost - fee_cost) * 100.0;

                if (net_arb >= min_arb_pct) {
                    ArbOpportunity opp;
                    opp.poly = poly;
                    opp.kalshi = kalshi;
                    opp.similarity = pair.similarity;
                    opp.direction = ArbDirection::POLY_YES_KALSHI_NO;
                    opp.poly_price = poly_cost;
                    opp.kalshi_price = kalshi_no_cost;
                    opp.arb_pct = net_arb;
                    opp.raw_arb_pct = raw_arb;
                    opp.profit_per_100 = net_arb;  // per $100 wagered on each side
                    result.opportunities.push_back(opp);
                }
            }
        }

        // Direction 2: Buy NO on Polymarket, Buy YES on Kalshi
        {
            double poly_cost = poly->no_price;
            double kalshi_yes_cost = kalshi->yes_price;

            if (poly_cost > 0.0 && kalshi_yes_cost > 0.0) {
                double total_cost = poly_cost + kalshi_yes_cost;
                double raw_arb = (1.0 - total_cost) * 100.0;
                double fee_cost = poly_cost * POLY_FEE + kalshi_yes_cost * KALSHI_FEE;
                double net_arb = (1.0 - total_cost - fee_cost) * 100.0;

                if (net_arb >= min_arb_pct) {
                    ArbOpportunity opp;
                    opp.poly = poly;
                    opp.kalshi = kalshi;
                    opp.similarity = pair.similarity;
                    opp.direction = ArbDirection::POLY_NO_KALSHI_YES;
                    opp.poly_price = poly_cost;
                    opp.kalshi_price = kalshi_yes_cost;
                    opp.arb_pct = net_arb;
                    opp.raw_arb_pct = raw_arb;
                    opp.profit_per_100 = net_arb;
                    result.opportunities.push_back(opp);
                }
            }
        }
    }

    // Sort by arb percentage descending
    std::sort(result.opportunities.begin(), result.opportunities.end(),
              [](const ArbOpportunity& a, const ArbOpportunity& b) {
                  return a.arb_pct > b.arb_pct;
              });

    auto end = std::chrono::steady_clock::now();
    result.calc_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    return result;
}

} // namespace arb
