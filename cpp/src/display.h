#pragma once

#include "arb.h"
#include "matcher.h"
#include <string>
#include <vector>

namespace arb {

// ANSI color codes
namespace color {
    constexpr const char* RESET   = "\033[0m";
    constexpr const char* BOLD    = "\033[1m";
    constexpr const char* DIM     = "\033[2m";
    constexpr const char* RED     = "\033[31m";
    constexpr const char* GREEN   = "\033[32m";
    constexpr const char* YELLOW  = "\033[33m";
    constexpr const char* BLUE    = "\033[34m";
    constexpr const char* MAGENTA = "\033[35m";
    constexpr const char* CYAN    = "\033[36m";
    constexpr const char* WHITE   = "\033[37m";
    constexpr const char* BG_GREEN  = "\033[42m";
    constexpr const char* BG_YELLOW = "\033[43m";
    constexpr const char* BG_RED    = "\033[41m";
}

// Display scan results in a colored table
void display_arb_table(const ArbResult& result,
                       int64_t poly_fetch_ms,
                       int64_t kalshi_fetch_ms,
                       int64_t match_ms,
                       int poly_count,
                       int kalshi_count,
                       int matched_count);

// Display matched markets list
void display_matched_markets(const MatchResult& result,
                             int64_t poly_fetch_ms,
                             int64_t kalshi_fetch_ms);

// Print a header banner
void print_banner();

// Clear terminal
void clear_screen();

// Truncate string to max_len with ellipsis
std::string truncate(const std::string& s, size_t max_len);

} // namespace arb
