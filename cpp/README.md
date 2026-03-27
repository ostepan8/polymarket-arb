# Polymarket-Kalshi Arbitrage Scanner (C++)

High-performance arbitrage scanner that finds price discrepancies between Polymarket and Kalshi prediction markets.

## Build

```bash
cd cpp
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

**Dependencies:**
- libcurl (system library)
- nlohmann/json (bundled in `include/json.hpp`)
- pthreads

## Usage

```bash
# One-shot scan
./build/arb scan

# Filter by minimum arb percentage
./build/arb scan --min-arb 2.0

# Filter by category (e.g., nba, election)
./build/arb scan --category election

# Continuous watch mode (10s refresh)
./build/arb watch --interval 10

# List all matched market pairs
./build/arb markets

# Full options
./build/arb scan --min-arb 1.0 --min-sim 0.4 --max-pages 10 --threads 24 --verbose
```

## Options

| Flag | Description | Default |
|------|-------------|---------|
| `--min-arb <pct>` | Minimum arb % to display | 0.0 |
| `--category <keyword>` | Filter by category keyword | (all) |
| `--interval <sec>` | Watch mode refresh interval | 30 |
| `--min-sim <val>` | Minimum match similarity (0-1) | 0.3 |
| `--max-pages <n>` | Max API pages to fetch | 50 |
| `--threads <n>` | Matching thread count | auto |
| `--verbose` | Show extra debug info | off |

## Architecture

- **Parallel fetching**: Polymarket and Kalshi APIs fetched simultaneously in separate threads
- **curl_multi**: Non-blocking parallel HTTP for paginated requests (batches of 50 concurrent connections)
- **Thread-pool matching**: Fuzzy string matching distributed across all CPU cores
- **Jaccard similarity**: Token-based matching with NBA team alias normalization
- **Arb calculation**: Checks both directions (Poly YES + Kalshi NO, Poly NO + Kalshi YES) with fee adjustment

## Fee Model

- Polymarket: 1%
- Kalshi: 3.8%
- Arb% shown is net of fees

## API Details

- **Polymarket**: `https://gamma-api.polymarket.com/markets` (public, no auth)
- **Kalshi**: `https://api.elections.kalshi.com/trade-api/v2` (public read, events + markets endpoints)
