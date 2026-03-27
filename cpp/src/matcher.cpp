#include "matcher.h"
#include <algorithm>
#include <cctype>
#include <sstream>
#include <thread>
#include <mutex>
#include <numeric>
#include <chrono>
#include <unordered_map>
#include <regex>

namespace arb {

namespace {

// Common stopwords to remove
const std::unordered_set<std::string> STOPWORDS = {
    "the", "a", "an", "and", "or", "but", "in", "on", "at", "to", "for",
    "of", "with", "by", "from", "will", "be", "is", "are", "was", "were",
    "has", "have", "had", "do", "does", "did", "this", "that", "it",
    "not", "no", "yes", "if", "than", "then", "so", "as", "its",
    "what", "which", "who", "whom", "when", "where", "how",
    "before", "after", "above", "below", "between", "during",
};

// NBA team name aliases
const std::unordered_map<std::string, std::string> NBA_ALIASES = {
    {"lakers", "lal"}, {"los angeles lakers", "lal"},
    {"celtics", "bos"}, {"boston celtics", "bos"},
    {"warriors", "gsw"}, {"golden state warriors", "gsw"}, {"golden state", "gsw"},
    {"bucks", "mil"}, {"milwaukee bucks", "mil"},
    {"76ers", "phi"}, {"sixers", "phi"}, {"philadelphia 76ers", "phi"},
    {"nets", "bkn"}, {"brooklyn nets", "bkn"},
    {"knicks", "nyk"}, {"new york knicks", "nyk"},
    {"heat", "mia"}, {"miami heat", "mia"},
    {"bulls", "chi"}, {"chicago bulls", "chi"},
    {"cavaliers", "cle"}, {"cavs", "cle"}, {"cleveland cavaliers", "cle"},
    {"hawks", "atl"}, {"atlanta hawks", "atl"},
    {"raptors", "tor"}, {"toronto raptors", "tor"},
    {"wizards", "was"}, {"washington wizards", "was"},
    {"pacers", "ind"}, {"indiana pacers", "ind"},
    {"magic", "orl"}, {"orlando magic", "orl"},
    {"hornets", "cha"}, {"charlotte hornets", "cha"},
    {"pistons", "det"}, {"detroit pistons", "det"},
    {"nuggets", "den"}, {"denver nuggets", "den"},
    {"suns", "phx"}, {"phoenix suns", "phx"},
    {"clippers", "lac"}, {"la clippers", "lac"}, {"los angeles clippers", "lac"},
    {"mavericks", "dal"}, {"mavs", "dal"}, {"dallas mavericks", "dal"},
    {"jazz", "uta"}, {"utah jazz", "uta"},
    {"blazers", "por"}, {"trail blazers", "por"}, {"portland trail blazers", "por"},
    {"timberwolves", "min"}, {"wolves", "min"}, {"minnesota timberwolves", "min"},
    {"thunder", "okc"}, {"oklahoma city thunder", "okc"},
    {"pelicans", "nop"}, {"new orleans pelicans", "nop"},
    {"kings", "sac"}, {"sacramento kings", "sac"},
    {"spurs", "sas"}, {"san antonio spurs", "sas"},
    {"grizzlies", "mem"}, {"memphis grizzlies", "mem"},
    {"rockets", "hou"}, {"houston rockets", "hou"},
};

} // anonymous namespace

std::string normalize_title(const std::string& title) {
    std::string result;
    result.reserve(title.size());

    for (char c : title) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            result += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else if (c == ' ' || c == '-' || c == '_') {
            if (!result.empty() && result.back() != ' ')
                result += ' ';
        }
    }

    // Trim trailing space
    while (!result.empty() && result.back() == ' ')
        result.pop_back();

    return result;
}

std::unordered_set<std::string> tokenize(const std::string& s) {
    std::unordered_set<std::string> tokens;
    std::istringstream iss(s);
    std::string word;
    while (iss >> word) {
        if (STOPWORDS.count(word) == 0 && word.size() > 1) {
            // Check NBA alias
            auto it = NBA_ALIASES.find(word);
            if (it != NBA_ALIASES.end()) {
                tokens.insert(it->second);
            }
            tokens.insert(word);
        }
    }
    return tokens;
}

double jaccard_similarity(const std::unordered_set<std::string>& a,
                          const std::unordered_set<std::string>& b) {
    if (a.empty() && b.empty()) return 0.0;

    size_t intersection = 0;
    // Iterate over the smaller set for efficiency
    const auto& smaller = (a.size() <= b.size()) ? a : b;
    const auto& larger = (a.size() <= b.size()) ? b : a;

    for (const auto& token : smaller) {
        if (larger.count(token)) intersection++;
    }

    size_t union_size = a.size() + b.size() - intersection;
    return union_size > 0 ? static_cast<double>(intersection) / static_cast<double>(union_size) : 0.0;
}

MatchResult match_markets(const std::vector<PolyMarket>& poly,
                          const std::vector<KalshiMarket>& kalshi,
                          const std::string& category_filter,
                          double min_similarity,
                          int num_threads) {
    MatchResult result;
    auto start = std::chrono::steady_clock::now();

    result.poly_count = static_cast<int>(poly.size());
    result.kalshi_count = static_cast<int>(kalshi.size());

    if (poly.empty() || kalshi.empty()) {
        auto end = std::chrono::steady_clock::now();
        result.match_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        return result;
    }

    // Determine thread count
    if (num_threads <= 0) {
        num_threads = static_cast<int>(std::thread::hardware_concurrency());
        if (num_threads <= 0) num_threads = 4;
    }

    // Pre-compute normalized titles and token sets
    struct NormalizedMarket {
        std::string normalized;
        std::unordered_set<std::string> tokens;
        bool passes_filter = true;
    };

    std::vector<NormalizedMarket> poly_norm(poly.size());
    std::vector<NormalizedMarket> kalshi_norm(kalshi.size());

    std::string cat_lower;
    if (!category_filter.empty()) {
        cat_lower = category_filter;
        std::transform(cat_lower.begin(), cat_lower.end(), cat_lower.begin(),
                       [](unsigned char c){ return std::tolower(c); });
    }

    // Parallel pre-processing of Polymarket titles
    auto preprocess_poly = [&](int thread_id) {
        for (size_t i = thread_id; i < poly.size(); i += num_threads) {
            poly_norm[i].normalized = normalize_title(poly[i].question);
            poly_norm[i].tokens = tokenize(poly_norm[i].normalized);
            if (!cat_lower.empty()) {
                poly_norm[i].passes_filter =
                    poly_norm[i].normalized.find(cat_lower) != std::string::npos;
            }
        }
    };

    auto preprocess_kalshi = [&](int thread_id) {
        for (size_t i = thread_id; i < kalshi.size(); i += num_threads) {
            // Combine event_title and title for better matching
            std::string combined = kalshi[i].event_title + " " + kalshi[i].title;
            kalshi_norm[i].normalized = normalize_title(combined);
            kalshi_norm[i].tokens = tokenize(kalshi_norm[i].normalized);
            if (!cat_lower.empty()) {
                std::string cat_check = kalshi[i].category;
                std::transform(cat_check.begin(), cat_check.end(), cat_check.begin(),
                               [](unsigned char c){ return std::tolower(c); });
                kalshi_norm[i].passes_filter =
                    kalshi_norm[i].normalized.find(cat_lower) != std::string::npos ||
                    cat_check.find(cat_lower) != std::string::npos;
            }
        }
    };

    // Launch preprocessing threads
    {
        std::vector<std::thread> threads;
        for (int t = 0; t < num_threads; t++) {
            threads.emplace_back(preprocess_poly, t);
            threads.emplace_back(preprocess_kalshi, t);
        }
        for (auto& th : threads) th.join();
    }

    // Build filtered index lists
    std::vector<size_t> poly_idx, kalshi_idx;
    poly_idx.reserve(poly.size());
    kalshi_idx.reserve(kalshi.size());

    for (size_t i = 0; i < poly.size(); i++) {
        if (poly_norm[i].passes_filter && !poly_norm[i].tokens.empty())
            poly_idx.push_back(i);
    }
    for (size_t i = 0; i < kalshi.size(); i++) {
        if (kalshi_norm[i].passes_filter && !kalshi_norm[i].tokens.empty())
            kalshi_idx.push_back(i);
    }

    // Parallel matching: each thread processes a chunk of poly markets
    std::mutex result_mutex;
    std::vector<MatchedPair> all_pairs;

    auto match_chunk = [&](size_t start_idx, size_t end_idx) {
        std::vector<MatchedPair> local_pairs;

        for (size_t pi = start_idx; pi < end_idx; pi++) {
            size_t i = poly_idx[pi];
            double best_sim = 0.0;
            size_t best_j = 0;

            for (size_t kj = 0; kj < kalshi_idx.size(); kj++) {
                size_t j = kalshi_idx[kj];
                double sim = jaccard_similarity(poly_norm[i].tokens, kalshi_norm[j].tokens);
                if (sim > best_sim) {
                    best_sim = sim;
                    best_j = j;
                }
            }

            if (best_sim >= min_similarity) {
                local_pairs.push_back({&poly[i], &kalshi[best_j], best_sim});
            }
        }

        std::lock_guard<std::mutex> lock(result_mutex);
        all_pairs.insert(all_pairs.end(), local_pairs.begin(), local_pairs.end());
    };

    // Distribute poly_idx across threads
    {
        std::vector<std::thread> threads;
        size_t chunk = (poly_idx.size() + num_threads - 1) / num_threads;
        for (int t = 0; t < num_threads; t++) {
            size_t s = t * chunk;
            size_t e = std::min(s + chunk, poly_idx.size());
            if (s < e) {
                threads.emplace_back(match_chunk, s, e);
            }
        }
        for (auto& th : threads) th.join();
    }

    // Sort by similarity descending
    std::sort(all_pairs.begin(), all_pairs.end(),
              [](const MatchedPair& a, const MatchedPair& b) {
                  return a.similarity > b.similarity;
              });

    // Deduplicate: each Kalshi market should appear at most once (best match wins)
    std::unordered_set<std::string> seen_kalshi;
    for (auto& p : all_pairs) {
        if (seen_kalshi.insert(p.kalshi->ticker).second) {
            result.pairs.push_back(p);
        }
    }

    auto end = std::chrono::steady_clock::now();
    result.match_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    return result;
}

} // namespace arb
