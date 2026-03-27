#include "polymarket.h"
#include "kalshi.h"
#include "matcher.h"
#include "arb.h"
#include "display.h"

#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <curl/curl.h>

using namespace arb;

static std::atomic<bool> g_running{true};

static void signal_handler(int) {
    g_running = false;
}

struct Config {
    std::string command = "scan";       // scan, watch, markets
    double min_arb = 0.0;              // minimum arb % to display
    std::string category;               // category filter
    int interval = 30;                  // watch mode interval (seconds)
    double min_similarity = 0.3;        // matching threshold
    int max_pages = 50;                 // max API pages to fetch
    int num_threads = 0;                // 0 = auto-detect
    bool no_color = false;
    bool verbose = false;
};

static void print_usage() {
    std::cout << color::BOLD << "Usage:" << color::RESET << "\n"
              << "  arb scan [options]     One-shot arbitrage scan\n"
              << "  arb watch [options]    Continuous monitoring mode\n"
              << "  arb markets [options]  List all matched market pairs\n"
              << "  arb help               Show this help\n"
              << "\n"
              << color::BOLD << "Options:" << color::RESET << "\n"
              << "  --min-arb <pct>       Minimum arb % to show (default: 0.0)\n"
              << "  --category <cat>      Filter by category keyword (e.g., nba, election)\n"
              << "  --interval <sec>      Watch mode refresh interval (default: 30)\n"
              << "  --min-sim <val>       Minimum similarity threshold 0-1 (default: 0.3)\n"
              << "  --max-pages <n>       Max API pages to fetch (default: 50)\n"
              << "  --threads <n>         Number of matching threads (default: auto)\n"
              << "  --no-color            Disable colored output\n"
              << "  --verbose             Show extra debug info\n"
              << "\n"
              << color::BOLD << "Examples:" << color::RESET << "\n"
              << "  arb scan                        # One-shot scan, show all\n"
              << "  arb scan --min-arb 2.0          # Only show 2%+ arbs\n"
              << "  arb scan --category nba         # NBA markets only\n"
              << "  arb watch --interval 10         # Watch mode, 10s refresh\n"
              << "  arb markets                     # List all matched pairs\n"
              << "\n";
}

static Config parse_args(int argc, char* argv[]) {
    Config cfg;

    if (argc < 2) {
        cfg.command = "scan";
        return cfg;
    }

    cfg.command = argv[1];

    for (int i = 2; i < argc; i++) {
        std::string arg = argv[i];

        if (arg == "--min-arb" && i + 1 < argc) {
            cfg.min_arb = std::stod(argv[++i]);
        } else if (arg == "--category" && i + 1 < argc) {
            cfg.category = argv[++i];
        } else if (arg == "--interval" && i + 1 < argc) {
            cfg.interval = std::stoi(argv[++i]);
        } else if (arg == "--min-sim" && i + 1 < argc) {
            cfg.min_similarity = std::stod(argv[++i]);
        } else if (arg == "--max-pages" && i + 1 < argc) {
            cfg.max_pages = std::stoi(argv[++i]);
        } else if (arg == "--threads" && i + 1 < argc) {
            cfg.num_threads = std::stoi(argv[++i]);
        } else if (arg == "--no-color") {
            cfg.no_color = true;
        } else if (arg == "--verbose") {
            cfg.verbose = true;
        }
    }

    return cfg;
}

static void run_scan(const Config& cfg, bool show_banner = true) {
    if (show_banner) print_banner();

    std::cout << color::DIM << "  Fetching markets from both exchanges..." << color::RESET << "\n";

    // Fetch from both exchanges in parallel
    FetchResult poly_result;
    KalshiFetchResult kalshi_result;

    std::thread poly_thread([&]() {
        poly_result = fetch_polymarket_markets(cfg.max_pages);
    });

    std::thread kalshi_thread([&]() {
        kalshi_result = fetch_kalshi_markets(cfg.max_pages);
    });

    poly_thread.join();
    kalshi_thread.join();

    // Report errors
    if (!poly_result.error.empty()) {
        std::cerr << color::RED << "  Polymarket error: " << poly_result.error
                  << color::RESET << "\n";
    }
    if (!kalshi_result.error.empty()) {
        std::cerr << color::RED << "  Kalshi error: " << kalshi_result.error
                  << color::RESET << "\n";
    }

    if (cfg.verbose) {
        std::cout << color::DIM
                  << "  Poly: " << poly_result.markets.size() << " markets ("
                  << poly_result.pages_fetched << " pages, " << poly_result.fetch_time_ms << "ms)\n"
                  << "  Kalshi: " << kalshi_result.markets.size() << " markets ("
                  << kalshi_result.pages_fetched << " pages, " << kalshi_result.fetch_time_ms << "ms)\n"
                  << color::RESET;
    }

    // Match markets
    auto match_result = match_markets(poly_result.markets, kalshi_result.markets,
                                       cfg.category, cfg.min_similarity, cfg.num_threads);

    // Calculate arbs
    auto arb_result = calculate_arbs(match_result.pairs, cfg.min_arb);

    // Display results
    display_arb_table(arb_result,
                      poly_result.fetch_time_ms,
                      kalshi_result.fetch_time_ms,
                      match_result.match_time_ms,
                      static_cast<int>(poly_result.markets.size()),
                      static_cast<int>(kalshi_result.markets.size()),
                      static_cast<int>(match_result.pairs.size()));
}

static void run_watch(const Config& cfg) {
    print_banner();
    std::cout << color::CYAN << "  Watch mode: refreshing every " << cfg.interval
              << "s (Ctrl+C to stop)" << color::RESET << "\n\n";

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    int iteration = 0;
    while (g_running) {
        iteration++;
        clear_screen();
        print_banner();
        std::cout << color::CYAN << "  Watch mode: iteration #" << iteration
                  << " | refreshing every " << cfg.interval << "s | Ctrl+C to stop"
                  << color::RESET << "\n\n";

        run_scan(cfg, false);

        // Sleep in 1-second increments so Ctrl+C is responsive
        for (int i = 0; i < cfg.interval && g_running; i++) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    std::cout << "\n" << color::YELLOW << "  Stopped." << color::RESET << "\n";
}

static void run_markets(const Config& cfg) {
    print_banner();
    std::cout << color::DIM << "  Fetching markets from both exchanges..." << color::RESET << "\n";

    FetchResult poly_result;
    KalshiFetchResult kalshi_result;

    std::thread poly_thread([&]() {
        poly_result = fetch_polymarket_markets(cfg.max_pages);
    });

    std::thread kalshi_thread([&]() {
        kalshi_result = fetch_kalshi_markets(cfg.max_pages);
    });

    poly_thread.join();
    kalshi_thread.join();

    if (!poly_result.error.empty()) {
        std::cerr << color::RED << "  Polymarket error: " << poly_result.error
                  << color::RESET << "\n";
    }
    if (!kalshi_result.error.empty()) {
        std::cerr << color::RED << "  Kalshi error: " << kalshi_result.error
                  << color::RESET << "\n";
    }

    auto match_result = match_markets(poly_result.markets, kalshi_result.markets,
                                       cfg.category, cfg.min_similarity, cfg.num_threads);

    display_matched_markets(match_result,
                            poly_result.fetch_time_ms,
                            kalshi_result.fetch_time_ms);
}

int main(int argc, char* argv[]) {
    // Global curl init (once)
    curl_global_init(CURL_GLOBAL_ALL);

    Config cfg = parse_args(argc, argv);

    if (cfg.command == "help" || cfg.command == "--help" || cfg.command == "-h") {
        print_banner();
        print_usage();
    } else if (cfg.command == "scan") {
        run_scan(cfg);
    } else if (cfg.command == "watch") {
        run_watch(cfg);
    } else if (cfg.command == "markets") {
        run_markets(cfg);
    } else {
        std::cerr << color::RED << "Unknown command: " << cfg.command << color::RESET << "\n\n";
        print_usage();
        curl_global_cleanup();
        return 1;
    }

    curl_global_cleanup();
    return 0;
}
