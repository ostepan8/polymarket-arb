"""Polymarket CLOB API client.

Fetches markets from the Polymarket Gamma API (public, no auth required for reads).
Uses the CLOB API for price data when an API key is available.

API Docs: https://docs.polymarket.com/#introduction
Gamma API: https://gamma-api.polymarket.com/markets
CLOB API: https://clob.polymarket.com
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
class PolymarketMarket:
    """A Polymarket binary market with YES/NO prices."""

    question: str
    yes_price: float        # 0-100 scale (cents)
    no_price: float         # 0-100 scale (cents)
    slug: str = ""
    condition_id: str = ""
    end_date: str = ""
    active: bool = True
    volume_24h: float = 0.0
    tags: tuple[str, ...] = ()

    @property
    def title(self) -> str:
        return self.question

    @property
    def url(self) -> str:
        return f"https://polymarket.com/event/{self.slug}" if self.slug else ""


# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

_GAMMA_URL = "https://gamma-api.polymarket.com/markets"
_CLOB_URL = "https://clob.polymarket.com"
_TIMEOUT = 15
_PAGE_SIZE = 100
_RATE_LIMIT_DELAY = 1.0  # seconds between requests

_HEADERS = {
    "Accept": "application/json",
    "User-Agent": "polymarket-arb-scanner/1.0",
}

# Category keyword mappings for filtering
_CATEGORY_KEYWORDS: dict[str, list[str]] = {
    "nba": ["nba", "basketball", "lakers", "celtics", "warriors", "nets",
            "knicks", "bucks", "76ers", "heat", "nuggets", "suns", "mavs",
            "clippers", "rockets", "thunder", "cavaliers", "grizzlies",
            "timberwolves", "pacers", "hawks", "bulls", "pistons", "magic",
            "hornets", "wizards", "blazers", "spurs", "kings", "jazz",
            "pelicans", "raptors"],
    "nfl": ["nfl", "football", "super bowl", "touchdown", "quarterback",
            "chiefs", "eagles", "49ers", "cowboys", "bills", "ravens",
            "dolphins", "lions", "packers"],
    "politics": ["president", "election", "trump", "biden", "congress",
                 "senate", "democrat", "republican", "vote", "governor",
                 "political", "white house"],
    "crypto": ["bitcoin", "ethereum", "btc", "eth", "crypto", "blockchain",
               "token", "defi", "solana", "sol"],
    "sports": ["nba", "nfl", "mlb", "nhl", "soccer", "tennis", "golf",
               "ufc", "boxing", "olympics", "championship", "playoff",
               "world series", "super bowl", "finals"],
}


# ---------------------------------------------------------------------------
# Client
# ---------------------------------------------------------------------------

class PolymarketClient:
    """Client for fetching Polymarket market data."""

    def __init__(self, api_key: str = ""):
        self.api_key = api_key
        self._session = requests.Session()
        self._session.headers.update(_HEADERS)
        if api_key:
            self._session.headers["Authorization"] = f"Bearer {api_key}"
        self._last_request_time = 0.0

    def _rate_limit(self):
        """Enforce rate limiting (1 req/sec)."""
        now = time.time()
        elapsed = now - self._last_request_time
        if elapsed < _RATE_LIMIT_DELAY:
            time.sleep(_RATE_LIMIT_DELAY - elapsed)
        self._last_request_time = time.time()

    def _parse_market(self, raw: dict) -> PolymarketMarket | None:
        """Parse a single Gamma API market response."""
        question = raw.get("question", "").strip()
        if not question:
            return None

        outcomes_str = raw.get("outcomes")
        prices_str = raw.get("outcomePrices")

        if not outcomes_str or not prices_str:
            return None

        try:
            outcomes = json.loads(outcomes_str) if isinstance(outcomes_str, str) else outcomes_str
            prices = json.loads(prices_str) if isinstance(prices_str, str) else prices_str
        except (json.JSONDecodeError, TypeError):
            return None

        # Only binary markets
        if len(outcomes) != 2 or len(prices) != 2:
            return None

        try:
            price_a = float(prices[0])
            price_b = float(prices[1])
        except (ValueError, TypeError):
            return None

        # Convert 0.0-1.0 to cents (0-100)
        yes_cents = round(price_a * 100, 2)
        no_cents = round(price_b * 100, 2)

        # Skip zero-priced markets
        if yes_cents < 0.5 and no_cents < 0.5:
            return None

        # Extract tags
        tags_raw = raw.get("tags", [])
        if isinstance(tags_raw, str):
            try:
                tags_raw = json.loads(tags_raw)
            except (json.JSONDecodeError, TypeError):
                tags_raw = []
        tags = tuple(str(t).lower() for t in tags_raw if t) if tags_raw else ()

        # Volume
        try:
            volume_24h = float(raw.get("volume24hr", 0) or 0)
        except (ValueError, TypeError):
            volume_24h = 0.0

        return PolymarketMarket(
            question=question,
            yes_price=yes_cents,
            no_price=no_cents,
            slug=raw.get("slug", ""),
            condition_id=raw.get("conditionId", ""),
            end_date=raw.get("endDate", ""),
            active=raw.get("active", True),
            volume_24h=volume_24h,
            tags=tags,
        )

    def _matches_category(self, market: PolymarketMarket, category: str) -> bool:
        """Check if a market matches a category filter."""
        cat_lower = category.lower()
        keywords = _CATEGORY_KEYWORDS.get(cat_lower, [cat_lower])

        searchable = market.question.lower()
        tag_str = " ".join(market.tags)

        return any(kw in searchable or kw in tag_str for kw in keywords)

    def fetch_markets(
        self,
        max_pages: int = 3,
        category: str | None = None,
    ) -> list[PolymarketMarket]:
        """Fetch active binary markets from Polymarket.

        Args:
            max_pages: Maximum API pages to fetch.
            category: Optional category filter (e.g., "nba", "politics").

        Returns:
            List of PolymarketMarket objects sorted by 24h volume desc.
        """
        markets: list[PolymarketMarket] = []
        offset = 0

        for _page in range(max_pages):
            self._rate_limit()

            params = {
                "closed": "false",
                "active": "true",
                "limit": _PAGE_SIZE,
                "offset": offset,
                "order": "volume24hr",
                "ascending": "false",
            }

            try:
                resp = self._session.get(
                    _GAMMA_URL,
                    params=params,
                    timeout=_TIMEOUT,
                )
                resp.raise_for_status()
            except requests.RequestException as exc:
                logger.error("Polymarket API error (offset=%d): %s", offset, exc)
                break

            try:
                data = resp.json()
            except (json.JSONDecodeError, ValueError) as exc:
                logger.error("Polymarket invalid JSON: %s", exc)
                break

            if not isinstance(data, list) or len(data) == 0:
                break

            for raw in data:
                parsed = self._parse_market(raw)
                if parsed is not None:
                    if category is None or self._matches_category(parsed, category):
                        markets.append(parsed)

            # End of results
            if len(data) < _PAGE_SIZE:
                break

            offset += _PAGE_SIZE

        return markets

    def fetch_prices(self, token_ids: list[str]) -> dict[str, float]:
        """Fetch current prices for specific token IDs from CLOB API.

        Returns dict mapping token_id -> price (0.0-1.0).
        """
        if not token_ids:
            return {}

        self._rate_limit()

        try:
            resp = self._session.get(
                f"{_CLOB_URL}/prices",
                params={"token_ids": ",".join(token_ids)},
                timeout=_TIMEOUT,
            )
            resp.raise_for_status()
            data = resp.json()
        except (requests.RequestException, json.JSONDecodeError) as exc:
            logger.error("CLOB price fetch error: %s", exc)
            return {}

        prices = {}
        for tid, price_data in data.items():
            try:
                prices[tid] = float(price_data.get("price", 0))
            except (ValueError, TypeError, AttributeError):
                pass

        return prices
