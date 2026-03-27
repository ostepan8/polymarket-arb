#pragma once

#include <string>
#include <vector>
#include <chrono>

namespace arb {

struct KalshiMarket {
    std::string ticker;
    std::string event_ticker;
    std::string title;
    std::string event_title;
    std::string category;
    std::string close_time;
    double yes_bid = 0.0;
    double yes_ask = 0.0;
    double no_bid = 0.0;
    double no_ask = 0.0;
    double volume = 0.0;
    // Derived
    double yes_price = 0.0;  // midpoint or best available
    double no_price = 0.0;
};

struct KalshiFetchResult {
    std::vector<KalshiMarket> markets;
    int64_t fetch_time_ms = 0;
    int pages_fetched = 0;
    std::string error;
};

// Fetch all open Kalshi events/markets using cursor-based pagination
KalshiFetchResult fetch_kalshi_markets(int max_pages = 50, int limit_per_page = 100);

} // namespace arb
