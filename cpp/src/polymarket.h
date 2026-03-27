#pragma once

#include <string>
#include <vector>
#include <functional>
#include <chrono>

namespace arb {

struct PolyToken {
    std::string token_id;
    std::string outcome; // "Yes" or "No"
    double price = 0.0;
};

struct PolyMarket {
    std::string id;
    std::string question;
    std::string slug;
    std::string category;
    std::string end_date;
    double volume = 0.0;
    bool active = false;
    std::vector<PolyToken> tokens;
    // Derived
    double yes_price = 0.0;
    double no_price = 0.0;
};

struct FetchResult {
    std::vector<PolyMarket> markets;
    int64_t fetch_time_ms = 0;
    int pages_fetched = 0;
    std::string error;
};

// Fetch all active Polymarket markets using curl_multi for parallel pagination
FetchResult fetch_polymarket_markets(int max_pages = 50, int limit_per_page = 100);

} // namespace arb
