#include "display.h"
#include <iostream>
#include <iomanip>
#include <sstream>
#include <cstdio>
#include <algorithm>

namespace arb {

std::string truncate(const std::string& s, size_t max_len) {
    if (s.size() <= max_len) return s;
    return s.substr(0, max_len - 3) + "...";
}

void print_banner() {
    std::cout << color::BOLD << color::CYAN;
    std::cout << R"(
  ____       _         _         _
 |  _ \ ___ | |_   _  / \   _ __| |__
 | |_) / _ \| | | | |/ _ \ | '__| '_ \
 |  __/ (_) | | |_| / ___ \| |  | |_) |
 |_|   \___/|_|\__, /_/   \_\_|  |_.__/
               |___/
  Polymarket-Kalshi Arbitrage Scanner v1.0
)";
    std::cout << color::RESET << "\n";
}

void clear_screen() {
    std::cout << "\033[2J\033[H";
}

static std::string format_pct(double pct) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << pct << "%";
    return oss.str();
}

static std::string format_price(double price) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << (price * 100) << "c";
    return oss.str();
}

static const char* arb_color(double arb_pct) {
    if (arb_pct >= 3.0) return color::GREEN;
    if (arb_pct >= 0.0) return color::YELLOW;
    return color::RED;
}

void display_arb_table(const ArbResult& result,
                       int64_t poly_fetch_ms,
                       int64_t kalshi_fetch_ms,
                       int64_t match_ms,
                       int poly_count,
                       int kalshi_count,
                       int matched_count) {
    // Timing header
    std::cout << color::DIM << "  Fetch Polymarket: " << poly_fetch_ms << "ms"
              << "  |  Fetch Kalshi: " << kalshi_fetch_ms << "ms"
              << "  |  Match: " << match_ms << "ms"
              << "  |  Arb calc: " << result.calc_time_ms << "ms"
              << color::RESET << "\n";

    std::cout << color::DIM << "  Polymarket: " << poly_count << " markets"
              << "  |  Kalshi: " << kalshi_count << " markets"
              << "  |  Matched: " << matched_count << " pairs"
              << "  |  Checked: " << result.total_pairs_checked << " pairs"
              << color::RESET << "\n\n";

    if (result.opportunities.empty()) {
        std::cout << color::YELLOW << "  No arbitrage opportunities found above threshold.\n"
                  << color::RESET;
        std::cout << color::DIM << "  Try lowering --min-arb or broadening the category filter.\n"
                  << color::RESET << "\n";
        return;
    }

    // Count profitable
    int profitable = 0;
    for (auto& o : result.opportunities) {
        if (o.arb_pct > 0.0) profitable++;
    }

    // Table header
    const int col_num = 4;
    const int col_market = 42;
    const int col_poly = 10;
    const int col_kalshi = 10;
    const int col_arb = 10;
    const int col_raw = 10;
    const int col_dir = 24;
    const int col_sim = 6;

    std::cout << color::BOLD << "  "
              << std::left << std::setw(col_num) << "#"
              << std::setw(col_market) << "Market"
              << std::setw(col_poly) << "Poly"
              << std::setw(col_kalshi) << "Kalshi"
              << std::setw(col_arb) << "Arb%"
              << std::setw(col_raw) << "Raw%"
              << std::setw(col_dir) << "Direction"
              << std::setw(col_sim) << "Sim"
              << color::RESET << "\n";

    // Separator
    std::cout << "  " << std::string(col_num + col_market + col_poly + col_kalshi +
                                      col_arb + col_raw + col_dir + col_sim, '-') << "\n";

    int idx = 0;
    for (auto& opp : result.opportunities) {
        idx++;
        const char* clr = arb_color(opp.arb_pct);

        std::string market_name = truncate(opp.poly->question, col_market - 2);

        std::cout << clr << "  "
                  << std::left << std::setw(col_num) << idx
                  << std::setw(col_market) << market_name
                  << std::setw(col_poly) << format_price(opp.poly_price)
                  << std::setw(col_kalshi) << format_price(opp.kalshi_price)
                  << std::setw(col_arb) << format_pct(opp.arb_pct)
                  << std::setw(col_raw) << format_pct(opp.raw_arb_pct)
                  << std::setw(col_dir) << direction_str(opp.direction)
                  << std::fixed << std::setprecision(2) << opp.similarity
                  << color::RESET << "\n";
    }

    std::cout << "\n";

    // Summary
    int64_t total_ms = poly_fetch_ms + kalshi_fetch_ms + match_ms + result.calc_time_ms;
    std::cout << color::BOLD;
    if (profitable > 0)
        std::cout << color::GREEN;
    else
        std::cout << color::YELLOW;

    std::cout << "  Found " << profitable << " profitable arb"
              << (profitable != 1 ? "s" : "")
              << " across " << matched_count << " matched markets in "
              << total_ms << "ms" << color::RESET << "\n";

    if (profitable > 0) {
        double best = result.opportunities[0].arb_pct;
        std::cout << color::GREEN << "  Best opportunity: "
                  << format_pct(best) << " on \""
                  << truncate(result.opportunities[0].poly->question, 60) << "\""
                  << color::RESET << "\n";
    }
    std::cout << "\n";
}

void display_matched_markets(const MatchResult& result,
                             int64_t poly_fetch_ms,
                             int64_t kalshi_fetch_ms) {
    std::cout << color::DIM << "  Fetch Polymarket: " << poly_fetch_ms << "ms"
              << "  |  Fetch Kalshi: " << kalshi_fetch_ms << "ms"
              << "  |  Match: " << result.match_time_ms << "ms"
              << color::RESET << "\n";
    std::cout << color::DIM << "  Polymarket: " << result.poly_count << " markets"
              << "  |  Kalshi: " << result.kalshi_count << " markets"
              << color::RESET << "\n\n";

    if (result.pairs.empty()) {
        std::cout << color::YELLOW << "  No matched markets found.\n" << color::RESET << "\n";
        return;
    }

    const int col_num = 4;
    const int col_poly = 46;
    const int col_kalshi = 46;
    const int col_sim = 8;
    const int col_pprice = 10;
    const int col_kprice = 10;

    std::cout << color::BOLD << "  "
              << std::left << std::setw(col_num) << "#"
              << std::setw(col_poly) << "Polymarket"
              << std::setw(col_kalshi) << "Kalshi"
              << std::setw(col_sim) << "Sim"
              << std::setw(col_pprice) << "P.Yes"
              << std::setw(col_kprice) << "K.Yes"
              << color::RESET << "\n";

    std::cout << "  " << std::string(col_num + col_poly + col_kalshi + col_sim +
                                      col_pprice + col_kprice, '-') << "\n";

    int idx = 0;
    for (auto& p : result.pairs) {
        idx++;
        const char* clr = p.similarity >= 0.6 ? color::GREEN :
                          p.similarity >= 0.4 ? color::YELLOW : color::DIM;

        std::cout << clr << "  "
                  << std::left << std::setw(col_num) << idx
                  << std::setw(col_poly) << truncate(p.poly->question, col_poly - 2)
                  << std::setw(col_kalshi) << truncate(p.kalshi->event_title + ": " + p.kalshi->title, col_kalshi - 2)
                  << std::fixed << std::setprecision(2) << std::setw(col_sim) << p.similarity
                  << std::setw(col_pprice) << format_price(p.poly->yes_price)
                  << std::setw(col_kprice) << format_price(p.kalshi->yes_price)
                  << color::RESET << "\n";
    }

    std::cout << "\n  " << color::BOLD << result.pairs.size() << " matched market pairs"
              << color::RESET << "\n\n";
}

} // namespace arb
