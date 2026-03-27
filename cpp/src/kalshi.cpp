#include "kalshi.h"
#include "json.hpp"
#include <curl/curl.h>
#include <cstring>
#include <algorithm>
#include <iostream>
#include <thread>
#include <mutex>
#include <unordered_map>

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

// Kalshi API base URL (migrated from trading-api.kalshi.com)
constexpr const char* KALSHI_BASE = "https://api.elections.kalshi.com/trade-api/v2";

double parse_dollar_string(const json& obj, const std::string& key) {
    if (!obj.contains(key)) return 0.0;
    if (obj[key].is_string()) {
        try { return std::stod(obj[key].get<std::string>()); }
        catch (...) { return 0.0; }
    }
    if (obj[key].is_number()) {
        double v = obj[key].get<double>();
        return v > 1.0 ? v / 100.0 : v;
    }
    return 0.0;
}

struct EventInfo {
    std::string event_ticker;
    std::string title;
    std::string category;
};

// Fetch all open events (cursor-paginated)
std::vector<EventInfo> fetch_events(int max_pages) {
    std::vector<EventInfo> events;
    events.reserve(2000);
    std::string cursor;

    for (int page = 0; page < max_pages; page++) {
        CURL* easy = curl_easy_init();
        if (!easy) break;

        CurlBuffer buf;
        std::string url = std::string(KALSHI_BASE) + "/events?limit=200&status=open";
        if (!cursor.empty()) url += "&cursor=" + cursor;

        curl_easy_setopt(easy, CURLOPT_URL, url.c_str());
        curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(easy, CURLOPT_WRITEDATA, &buf);
        curl_easy_setopt(easy, CURLOPT_TIMEOUT, 15L);
        curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 5L);
        curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(easy, CURLOPT_USERAGENT, "PolyArb/1.0");
        curl_easy_setopt(easy, CURLOPT_ACCEPT_ENCODING, "gzip");

        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Accept: application/json");
        curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers);

        CURLcode res = curl_easy_perform(easy);
        long http_code = 0;
        curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &http_code);
        curl_slist_free_all(headers);
        curl_easy_cleanup(easy);

        if (res != CURLE_OK || http_code != 200) break;

        try {
            auto j = json::parse(buf.data);
            cursor = j.value("cursor", "");
            auto& evts = j["events"];
            if (!evts.is_array() || evts.empty()) break;

            for (auto& e : evts) {
                EventInfo ei;
                ei.event_ticker = e.value("event_ticker", "");
                ei.title = e.value("title", "");
                ei.category = e.value("category", "");
                if (!ei.event_ticker.empty())
                    events.push_back(std::move(ei));
            }

            if (cursor.empty() || evts.size() < 200) break;
        } catch (...) { break; }
    }

    return events;
}

// Fetch markets for a batch of events using curl_multi
// Returns all non-MVE markets found
std::vector<KalshiMarket> fetch_markets_for_events(
    const std::vector<EventInfo>& events,
    size_t start, size_t end)
{
    std::vector<KalshiMarket> markets;
    if (start >= end || start >= events.size()) return markets;
    end = std::min(end, events.size());

    CURLM* multi = curl_multi_init();
    if (!multi) return markets;

    struct ReqCtx {
        CURL* easy = nullptr;
        CurlBuffer buf;
        size_t event_idx = 0;
    };

    size_t batch_size = end - start;
    std::vector<ReqCtx> reqs(batch_size);

    for (size_t i = 0; i < batch_size; i++) {
        size_t idx = start + i;
        reqs[i].event_idx = idx;
        reqs[i].easy = curl_easy_init();
        if (!reqs[i].easy) continue;

        std::string url = std::string(KALSHI_BASE) + "/markets?limit=50&event_ticker=" +
                          events[idx].event_ticker;

        curl_easy_setopt(reqs[i].easy, CURLOPT_URL, url.c_str());
        curl_easy_setopt(reqs[i].easy, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(reqs[i].easy, CURLOPT_WRITEDATA, &reqs[i].buf);
        curl_easy_setopt(reqs[i].easy, CURLOPT_TIMEOUT, 10L);
        curl_easy_setopt(reqs[i].easy, CURLOPT_CONNECTTIMEOUT, 5L);
        curl_easy_setopt(reqs[i].easy, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(reqs[i].easy, CURLOPT_USERAGENT, "PolyArb/1.0");
        curl_easy_setopt(reqs[i].easy, CURLOPT_ACCEPT_ENCODING, "gzip");

        curl_multi_add_handle(multi, reqs[i].easy);
    }

    // Execute all in parallel
    int still_running = 0;
    do {
        CURLMcode mc = curl_multi_perform(multi, &still_running);
        if (mc != CURLM_OK) break;
        if (still_running) {
            mc = curl_multi_poll(multi, nullptr, 0, 1000, nullptr);
            if (mc != CURLM_OK) break;
        }
    } while (still_running);

    // Parse results
    for (auto& req : reqs) {
        if (!req.easy) continue;

        long http_code = 0;
        curl_easy_getinfo(req.easy, CURLINFO_RESPONSE_CODE, &http_code);

        if (http_code == 200 && !req.buf.data.empty()) {
            try {
                auto j = json::parse(req.buf.data);
                auto& mkts = j["markets"];
                if (!mkts.is_array()) continue;

                const auto& evt = events[req.event_idx];

                for (auto& m : mkts) {
                    std::string ticker = m.value("ticker", "");
                    std::string event_ticker = m.value("event_ticker", "");

                    // Skip MVE/parlay markets
                    if (ticker.find("MVE") != std::string::npos) continue;
                    if (event_ticker.find("MVE") != std::string::npos) continue;

                    KalshiMarket km;
                    km.ticker = ticker;
                    km.event_ticker = event_ticker;
                    km.event_title = evt.title;
                    km.category = evt.category;
                    km.title = m.value("title", "");
                    km.close_time = m.value("close_time", m.value("expiration_time", ""));

                    // yes_sub_title has the specific outcome for multi-choice events
                    std::string yes_sub = m.value("yes_sub_title", "");
                    if (!yes_sub.empty() && yes_sub != km.title) {
                        km.title = km.title + " - " + yes_sub;
                    }

                    // Prices are dollar strings like "0.0900"
                    km.yes_bid = parse_dollar_string(m, "yes_bid_dollars");
                    km.yes_ask = parse_dollar_string(m, "yes_ask_dollars");
                    km.no_bid = parse_dollar_string(m, "no_bid_dollars");
                    km.no_ask = parse_dollar_string(m, "no_ask_dollars");

                    // Fallback to last_price
                    if (km.yes_bid == 0.0 && km.yes_ask == 0.0) {
                        double last = parse_dollar_string(m, "last_price_dollars");
                        if (last > 0.0) {
                            km.yes_bid = last;
                            km.yes_ask = last;
                        }
                    }

                    // Volume
                    km.volume = 0.0;
                    if (m.contains("volume_fp")) {
                        if (m["volume_fp"].is_string()) {
                            try { km.volume = std::stod(m["volume_fp"].get<std::string>()); }
                            catch (...) {}
                        } else if (m["volume_fp"].is_number()) {
                            km.volume = m["volume_fp"].get<double>();
                        }
                    }

                    // Derive yes/no prices
                    km.yes_price = (km.yes_bid > 0 && km.yes_ask > 0)
                                   ? (km.yes_bid + km.yes_ask) / 2.0
                                   : std::max(km.yes_bid, km.yes_ask);
                    km.no_price = (km.no_bid > 0 && km.no_ask > 0)
                                  ? (km.no_bid + km.no_ask) / 2.0
                                  : std::max(km.no_bid, km.no_ask);

                    if (km.yes_price > 0.0 && km.no_price == 0.0)
                        km.no_price = 1.0 - km.yes_price;
                    if (km.no_price > 0.0 && km.yes_price == 0.0)
                        km.yes_price = 1.0 - km.no_price;

                    if (km.yes_price > 0.0 || km.no_price > 0.0) {
                        markets.push_back(std::move(km));
                    }
                }
            } catch (...) {}
        }

        curl_multi_remove_handle(multi, req.easy);
        curl_easy_cleanup(req.easy);
    }

    curl_multi_cleanup(multi);
    return markets;
}

} // anonymous namespace

KalshiFetchResult fetch_kalshi_markets(int max_pages, int limit_per_page) {
    KalshiFetchResult result;
    auto start = std::chrono::steady_clock::now();

    // Step 1: Fetch all events (fast, lightweight)
    auto events = fetch_events(max_pages);

    if (events.empty()) {
        result.error = "No Kalshi events found";
        auto end = std::chrono::steady_clock::now();
        result.fetch_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        return result;
    }

    // Step 2: Fetch markets for all events in parallel batches
    // curl_multi handles up to ~100 connections well, so batch in groups of 50
    constexpr size_t BATCH_SIZE = 50;
    std::vector<KalshiMarket> all_markets;
    all_markets.reserve(events.size() * 2);

    int pages_fetched = 0;
    for (size_t i = 0; i < events.size(); i += BATCH_SIZE) {
        auto batch = fetch_markets_for_events(events, i, i + BATCH_SIZE);
        all_markets.insert(all_markets.end(),
                           std::make_move_iterator(batch.begin()),
                           std::make_move_iterator(batch.end()));
        pages_fetched++;
    }

    auto end = std::chrono::steady_clock::now();
    result.markets = std::move(all_markets);
    result.fetch_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    result.pages_fetched = pages_fetched;
    return result;
}

} // namespace arb
