# Polymarket-Kalshi Arbitrage Scanner

CLI tool to find arbitrage opportunities between Polymarket and Kalshi prediction markets.

## Setup
```bash
pip install -r requirements.txt
cp .env.example .env  # Add your API keys (optional for basic scanning)
```

## Usage
```bash
# Scan for arbitrage opportunities (default: >= 1% arb)
python arb.py scan

# Higher threshold
python arb.py scan --min-arb 3

# Filter by category
python arb.py scan --category politics
python arb.py scan --category nba

# Show matched market pairs alongside arbs
python arb.py scan --show-matches

# Fetch more pages for broader coverage
python arb.py scan --poly-pages 5 --kalshi-pages 10

# List all overlapping markets between exchanges
python arb.py markets
python arb.py markets --category sports

# Continuous monitoring (refreshes every 30s)
python arb.py watch
python arb.py watch --min-arb 2 --interval 60
```

## How It Works

1. Fetches active binary markets from both Polymarket (Gamma API) and Kalshi (Events API)
2. Matches markets across exchanges using fuzzy title matching (rapidfuzz) with keyword pre-filtering
3. For NBA markets: uses team name normalization and date matching for higher accuracy
4. Calculates arbitrage: if buying complementary positions (YES on one + NO on other) costs < 100c after fees, there's an arb
5. Accounts for exchange fees: Polymarket ~1%, Kalshi ~3.8%

## API Keys

All keys are optional for basic market scanning:
- `POLYMARKET_API_KEY` -- Polymarket CLOB API key (enhanced price data)
- `KALSHI_API_EMAIL` / `KALSHI_API_PASSWORD` -- Kalshi credentials (orderbook access)

## Project Structure
```
arb.py              # CLI entry point (click)
matcher.py          # Fuzzy matching + arb detection
clients/
  polymarket.py     # Polymarket Gamma/CLOB API client
  kalshi.py         # Kalshi Trading API client
```
