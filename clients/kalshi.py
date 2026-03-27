"""Kalshi Trading API client.

Fetches markets from the Kalshi public events API.
Optionally authenticates for orderbook data.

API Docs: https://trading-api.kalshi.com/trade-api/v2
"""

from __future__ import annotations

import json
import logging
import time
from dataclasses import dataclass

import requests

logger = logging.getLogger(__name__)


# ---------------------------------------------------------------------------
# Data types
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class KalshiMarket:
    """A Kalshi binary market with YES/NO prices."""

    title: str
    ticker: str
    yes_price: float        # 0-100 scale (cents)
    no_price: float         # 0-100 scale (cents)
    yes_bid: float = 0.0    # best bid (cents)
    yes_ask: float = 0.0    # best ask (cents)
    close_time: str = ""
    event_ticker: str = ""
    category: str = ""
    volume: int = 0

    @property
    def url(self) -> str:
        return f"https://kalshi.com/markets/{self.ticker}" if self.ticker else ""


# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

_BASE_URL = "https://api.elections.kalshi.com/trade-api/v2"
_TRADING_URL = "https://trading-api.kalshi.com/trade-api/v2"
_TIMEOUT = 15
_PAGE_SIZE = 100
_RATE_LIMIT_DELAY = 1.0  # seconds between requests

_HEADERS = {
    "Accept": "application/json",
    "Content-Type": "application/json",
    "User-Agent": "polymarket-arb-scanner/1.0",
}

# Category keyword mappings for filtering
_CATEGORY_KEYWORDS: dict[str, list[str]] = {
    "nba": ["nba", "basketball"],
    "nfl": ["nfl", "football", "super-bowl"],
    "politics": ["politics", "election", "president", "congress", "senate",
                 "governor"],
    "crypto": ["crypto", "bitcoin", "ethereum"],
    "sports": ["sports", "nba", "nfl", "mlb", "nhl", "soccer", "tennis",
               "golf", "ufc", "boxing"],
    "economy": ["economy", "fed", "interest-rate", "inflation", "gdp",
                "unemployment", "recession"],
    "weather": ["weather", "temperature", "hurricane", "tornado"],
}


# ---------------------------------------------------------------------------
# Client
# ---------------------------------------------------------------------------

class KalshiClient:
    """Client for fetching Kalshi market data."""

    def __init__(self, email: str = "", password: str = ""):
        self.email = email
        self.password = password
        self._session = requests.Session()
        self._session.headers.update(_HEADERS)
        self._auth_token: str | None = None
        self._last_request_time = 0.0

    def _rate_limit(self):
        """Enforce rate limiting (1 req/sec)."""
        now = time.time()
        elapsed = now - self._last_request_time
        if elapsed < _RATE_LIMIT_DELAY:
            time.sleep(_RATE_LIMIT_DELAY - elapsed)
        self._last_request_time = time.time()

    def _authenticate(self) -> bool:
        """Authenticate with Kalshi to get an auth token.

        Returns True on success, False otherwise.
        """
        if not self.email or not self.password:
            logger.debug("No Kalshi credentials — using unauthenticated access.")
            return False

        if self._auth_token:
            return True

        self._rate_limit()

        try:
            resp = self._session.post(
                f"{_TRADING_URL}/login",
                json={"email": self.email, "password": self.password},
                timeout=_TIMEOUT,
            )
            resp.raise_for_status()
            data = resp.json()
            self._auth_token = data.get("token", "")
            if self._auth_token:
                self._session.headers["Authorization"] = f"Bearer {self._auth_token}"
                logger.info("Kalshi authentication successful.")
                return True
        except requests.RequestException as exc:
            logger.warning("Kalshi auth failed: %s", exc)

        return False

    def _parse_market(self, raw: dict, event_title: str = "") -> KalshiMarket | None:
        """Parse a single Kalshi market from the events API response."""
        title = raw.get("title", "").strip()
        subtitle = raw.get("subtitle", "").strip()

        if not title:
            title = event_title
        if subtitle:
            title = f"{title} - {subtitle}"

        if not title or len(title) < 5:
            return None

        # Extract prices — Kalshi uses dollar strings like "0.5500"
        yes_bid_str = raw.get("yes_bid_dollars", "0")
        yes_ask_str = raw.get("yes_ask_dollars", "0")
        no_bid_str = raw.get("no_bid_dollars", "0")
        no_ask_str = raw.get("no_ask_dollars", "0")

        try:
            yes_bid = float(yes_bid_str or "0")
            yes_ask = float(yes_ask_str or "0")
            no_bid = float(no_bid_str or "0")
            no_ask = float(no_ask_str or "0")
        except (ValueError, TypeError):
            return None

        # Use midpoint for fair price
        yes_mid = (yes_bid + yes_ask) / 2 if (yes_bid + yes_ask) > 0 else 0
        no_mid = (no_bid + no_ask) / 2 if (no_bid + no_ask) > 0 else 0

        # Convert dollars to cents
        yes_cents = round(yes_mid * 100, 2)
        no_cents = round(no_mid * 100, 2)

        # Skip zero-priced
        if yes_cents < 0.5 and no_cents < 0.5:
            return None

        ticker = raw.get("ticker", "")
        event_ticker = raw.get("event_ticker", "")
        category = raw.get("category", "")
        close_time = raw.get("close_time", "")

        try:
            volume = int(raw.get("volume", 0) or 0)
        except (ValueError, TypeError):
            volume = 0

        return KalshiMarket(
            title=title,
            ticker=ticker,
            yes_price=yes_cents,
            no_price=no_cents,
            yes_bid=round(yes_bid * 100, 2),
            yes_ask=round(yes_ask * 100, 2),
            close_time=close_time,
            event_ticker=event_ticker,
            category=category,
            volume=volume,
        )

    def _matches_category(self, market: KalshiMarket, category: str) -> bool:
        """Check if a market matches a category filter."""
        cat_lower = category.lower()
        keywords = _CATEGORY_KEYWORDS.get(cat_lower, [cat_lower])

        searchable = f"{market.title} {market.category} {market.event_ticker}".lower()
        return any(kw in searchable for kw in keywords)

    def fetch_markets(
        self,
        max_pages: int = 5,
        category: str | None = None,
    ) -> list[KalshiMarket]:
        """Fetch active binary markets from Kalshi.

        Uses the events API with nested markets. No auth required for reads.

        Args:
            max_pages: Maximum API pages to fetch.
            category: Optional category filter.

        Returns:
            List of KalshiMarket objects.
        """
        # Try to authenticate (optional — more data with auth)
        self._authenticate()

        markets: list[KalshiMarket] = []
        cursor: str | None = None

        for _page in range(max_pages):
            self._rate_limit()

            params: dict[str, str | int] = {
                "limit": _PAGE_SIZE,
                "status": "open",
                "with_nested_markets": "true",
            }
            if cursor:
                params["cursor"] = cursor

            try:
                resp = self._session.get(
                    f"{_BASE_URL}/events",
                    params=params,
                    timeout=_TIMEOUT,
                )
                resp.raise_for_status()
            except requests.RequestException as exc:
                logger.error("Kalshi API error (page=%d): %s", _page, exc)
                break

            try:
                data = resp.json()
            except (json.JSONDecodeError, ValueError) as exc:
                logger.error("Kalshi invalid JSON: %s", exc)
                break

            events = data.get("events", [])
            if not events:
                break

            for event in events:
                event_title = event.get("title", "")
                nested = event.get("markets", [])

                for raw in nested:
                    # Only binary + active markets
                    if raw.get("market_type") != "binary":
                        continue
                    if raw.get("status") != "active":
                        continue
                    if raw.get("strike_type") == "custom":
                        continue

                    parsed = self._parse_market(raw, event_title)
                    if parsed is not None:
                        if category is None or self._matches_category(parsed, category):
                            markets.append(parsed)

            next_cursor = data.get("cursor", "")
            if not next_cursor or next_cursor == cursor:
                break
            cursor = next_cursor

        return markets

    def fetch_orderbook(self, ticker: str) -> dict:
        """Fetch the orderbook for a specific market ticker.

        Returns dict with 'yes' and 'no' keys containing bid/ask arrays.
        Requires authentication for best results.
        """
        self._rate_limit()

        try:
            resp = self._session.get(
                f"{_TRADING_URL}/markets/{ticker}/orderbook",
                timeout=_TIMEOUT,
            )
            resp.raise_for_status()
            return resp.json().get("orderbook", {})
        except (requests.RequestException, json.JSONDecodeError) as exc:
            logger.error("Kalshi orderbook error for %s: %s", ticker, exc)
            return {}
