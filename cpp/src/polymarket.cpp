#include "polymarket.h"
#include "json.hpp"
#include <curl/curl.h>
#include <mutex>
#include <cstring>
#include <algorithm>
#include <iostream>

using json = nlohmann::json;

namespace arb {

namespace {

struct CurlBuffer {
    std::string data;
};

size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* buf = static_cast<CurlBuffer*>(userdata);
    buf->data.append(ptr, size * nmemb);
    return size * nmemb;
}

constexpr const char* POLY_BASE = "https://gamma-api.polymarket.com";

} // anonymous namespace

FetchResult fetch_polymarket_markets(int max_pages, int limit_per_page) {
    FetchResult result;
    auto start = std::chrono::steady_clock::now();

    // Phase 1: Fire off first request to gauge how many markets exist
    // Phase 2: Fire all remaining pages in parallel via curl_multi

    CURLM* multi = curl_multi_init();
    if (!multi) {
        result.error = "Failed to init curl_multi";
        return result;
    }

    // We'll fetch pages in batches. Start with a generous batch.
    struct PageCtx {
        CURL* easy = nullptr;
        CurlBuffer buf;
        int page = 0;
        int offset = 0;
    };

    std::vector<PolyMarket> all_markets;
    all_markets.reserve(5000);

    int offset = 0;
    bool done = false;
    int total_pages = 0;

    while (!done && total_pages < max_pages) {
        // Create a batch of page requests
        int batch_size = std::min(10, max_pages - total_pages);
        std::vector<PageCtx> pages(batch_size);

        for (int i = 0; i < batch_size; i++) {
            pages[i].page = total_pages + i;
            pages[i].offset = offset + i * limit_per_page;
            pages[i].easy = curl_easy_init();

            std::string url = std::string(POLY_BASE) + "/markets?limit=" +
                              std::to_string(limit_per_page) +
                              "&offset=" + std::to_string(pages[i].offset) +
                              "&active=true&closed=false";

            curl_easy_setopt(pages[i].easy, CURLOPT_URL, url.c_str());
            curl_easy_setopt(pages[i].easy, CURLOPT_WRITEFUNCTION, write_callback);
            curl_easy_setopt(pages[i].easy, CURLOPT_WRITEDATA, &pages[i].buf);
            curl_easy_setopt(pages[i].easy, CURLOPT_TIMEOUT, 15L);
            curl_easy_setopt(pages[i].easy, CURLOPT_CONNECTTIMEOUT, 5L);
            curl_easy_setopt(pages[i].easy, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(pages[i].easy, CURLOPT_USERAGENT, "PolyArb/1.0");
            curl_easy_setopt(pages[i].easy, CURLOPT_ACCEPT_ENCODING, "gzip");

            curl_multi_add_handle(multi, pages[i].easy);
        }

        // Run all requests in parallel
        int still_running = 0;
        do {
            CURLMcode mc = curl_multi_perform(multi, &still_running);
            if (mc != CURLM_OK) break;
            if (still_running) {
                mc = curl_multi_poll(multi, nullptr, 0, 1000, nullptr);
                if (mc != CURLM_OK) break;
            }
        } while (still_running);

        // Process results
        bool batch_had_empty = false;
        for (auto& pg : pages) {
            long http_code = 0;
            curl_easy_getinfo(pg.easy, CURLINFO_RESPONSE_CODE, &http_code);

            if (http_code == 200 && !pg.buf.data.empty()) {
                try {
                    auto j = json::parse(pg.buf.data);
                    // Response is an array of markets
                    auto& arr = j.is_array() ? j : j["data"];
                    if (!arr.is_array() || arr.empty()) {
                        batch_had_empty = true;
                        continue;
                    }
                    if (arr.size() < static_cast<size_t>(limit_per_page)) {
                        batch_had_empty = true;
                    }

                    for (auto& m : arr) {
                        PolyMarket pm;
                        pm.id = m.value("id", "");
                        pm.question = m.value("question", "");
                        pm.slug = m.value("slug", "");
                        pm.end_date = m.value("end_date_iso", m.value("endDate", ""));
                        pm.active = m.value("active", true);
                        pm.volume = 0.0;

                        // Volume can be string or number
                        if (m.contains("volume")) {
                            if (m["volume"].is_string()) {
                                try { pm.volume = std::stod(m["volume"].get<std::string>()); }
                                catch (...) {}
                            } else if (m["volume"].is_number()) {
                                pm.volume = m["volume"].get<double>();
                            }
                        }

                        // Extract tokens
                        if (m.contains("tokens") && m["tokens"].is_array()) {
                            for (auto& t : m["tokens"]) {
                                PolyToken tok;
                                tok.token_id = t.value("token_id", "");
                                tok.outcome = t.value("outcome", "");
                                tok.price = t.value("price", 0.0);
                                if (tok.price == 0.0 && t.contains("price") && t["price"].is_string()) {
                                    try { tok.price = std::stod(t["price"].get<std::string>()); }
                                    catch (...) {}
                                }
                                pm.tokens.push_back(std::move(tok));
                            }
                        }

                        // Extract yes/no prices from tokens
                        for (auto& tok : pm.tokens) {
                            if (tok.outcome == "Yes") pm.yes_price = tok.price;
                            else if (tok.outcome == "No") pm.no_price = tok.price;
                        }

                        // Also check outcomePrices field (some responses)
                        if (pm.yes_price == 0.0 && m.contains("outcomePrices")) {
                            if (m["outcomePrices"].is_string()) {
                                try {
                                    auto prices = json::parse(m["outcomePrices"].get<std::string>());
                                    if (prices.is_array() && prices.size() >= 2) {
                                        if (prices[0].is_string())
                                            pm.yes_price = std::stod(prices[0].get<std::string>());
                                        else if (prices[0].is_number())
                                            pm.yes_price = prices[0].get<double>();
                                        if (prices[1].is_string())
                                            pm.no_price = std::stod(prices[1].get<std::string>());
                                        else if (prices[1].is_number())
                                            pm.no_price = prices[1].get<double>();
                                    }
                                } catch (...) {}
                            } else if (m["outcomePrices"].is_array()) {
                                auto& prices = m["outcomePrices"];
                                if (prices.size() >= 2) {
                                    if (prices[0].is_string())
                                        pm.yes_price = std::stod(prices[0].get<std::string>());
                                    else if (prices[0].is_number())
                                        pm.yes_price = prices[0].get<double>();
                                    if (prices[1].is_string())
                                        pm.no_price = std::stod(prices[1].get<std::string>());
                                    else if (prices[1].is_number())
                                        pm.no_price = prices[1].get<double>();
                                }
                            }
                        }

                        // Also check bestBid / bestAsk patterns
                        if (pm.yes_price == 0.0 && m.contains("bestBid")) {
                            pm.yes_price = m.value("bestBid", 0.0);
                        }

                        // Skip markets with no price data
                        if (pm.yes_price > 0.0 || pm.no_price > 0.0) {
                            // Derive missing price
                            if (pm.yes_price > 0.0 && pm.no_price == 0.0)
                                pm.no_price = 1.0 - pm.yes_price;
                            else if (pm.no_price > 0.0 && pm.yes_price == 0.0)
                                pm.yes_price = 1.0 - pm.no_price;

                            all_markets.push_back(std::move(pm));
                        }
                    }
                } catch (const std::exception& e) {
                    // Parse error, skip page
                }
            } else {
                batch_had_empty = true;
            }

            curl_multi_remove_handle(multi, pg.easy);
            curl_easy_cleanup(pg.easy);
        }

        total_pages += batch_size;
        offset += batch_size * limit_per_page;

        if (batch_had_empty) done = true;
    }

    curl_multi_cleanup(multi);

    auto end = std::chrono::steady_clock::now();
    result.markets = std::move(all_markets);
    result.fetch_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    result.pages_fetched = total_pages;
    return result;
}

} // namespace arb
