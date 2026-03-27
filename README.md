# Polymarket-Kalshi Arbitrage Scanner

CLI tool to find arbitrage opportunities between Polymarket and Kalshi prediction markets.

## Setup
```bash
pip install -r requirements.txt
cp .env.example .env  # Add your API keys
```

## Usage
```bash
python arb.py scan              # Scan for arbs
python arb.py scan --min-arb 2  # Min 2% arb
python arb.py watch             # Continuous monitoring
python arb.py markets           # List all overlapping markets
```

## API Keys
- `POLYMARKET_API_KEY` — Polymarket CLOB API key
- `KALSHI_API_EMAIL` / `KALSHI_API_PASSWORD` — Kalshi credentials
