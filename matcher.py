"""Market matching and arbitrage detection between Polymarket and Kalshi.

Uses rapidfuzz for efficient fuzzy string matching with keyword pre-filtering
to avoid O(n*m) expensive comparisons.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from typing import Sequence

from rapidfuzz import fuzz

from clients.polymarket import PolymarketMarket
from clients.kalshi import KalshiMarket


# ---------------------------------------------------------------------------
# Data types
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class MatchedMarket:
    """A pair of markets matched across exchanges."""

    poly_market: PolymarketMarket
    kalshi_market: KalshiMarket
    similarity: float  # 0.0 to 1.0


@dataclass(frozen=True)
class ArbOpportunity:
    """An arbitrage opportunity between two exchanges."""

    title: str
    poly_yes: float       # Polymarket YES price (cents)
    poly_no: float        # Polymarket NO price (cents)
    kalshi_yes: float     # Kalshi YES price (cents)
    kalshi_no: float      # Kalshi NO price (cents)
    arb_pct: float        # Arbitrage percentage after fees
    strategy: str         # Human-readable strategy description
    poly_url: str = ""
    kalshi_url: str = ""


# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

_MATCH_THRESHOLD = 0.55    # minimum similarity for a fuzzy match
_KEYWORD_OVERLAP_MIN = 2   # minimum shared keywords for pre-filter

# Stop words to exclude from keyword extraction
_STOP_WORDS = frozenset({
    "will", "the", "a", "an", "of", "in", "on", "to", "for", "by", "at",
    "be", "is", "it", "or", "and", "vs", "before", "after", "this", "that",
    "what", "who", "how", "yes", "no", "win", "before", "more", "than",
    "over", "under", "above", "below", "between", "during", "market",
})

# NBA team name normalization
_NBA_TEAMS: dict[str, str] = {
    "lakers": "lakers", "los angeles lakers": "lakers", "la lakers": "lakers",
    "celtics": "celtics", "boston celtics": "celtics", "boston": "celtics",
    "warriors": "warriors", "golden state warriors": "warriors", "golden state": "warriors",
    "nets": "nets", "brooklyn nets": "nets", "brooklyn": "nets",
    "knicks": "knicks", "new york knicks": "knicks",
    "bucks": "bucks", "milwaukee bucks": "bucks", "milwaukee": "bucks",
    "76ers": "76ers", "sixers": "76ers", "philadelphia 76ers": "76ers",
    "heat": "heat", "miami heat": "heat", "miami": "heat",
    "nuggets": "nuggets", "denver nuggets": "nuggets", "denver": "nuggets",
    "suns": "suns", "phoenix suns": "suns", "phoenix": "suns",
    "mavericks": "mavericks", "mavs": "mavericks", "dallas mavericks": "mavericks",
    "clippers": "clippers", "la clippers": "clippers",
    "rockets": "rockets", "houston rockets": "rockets", "houston": "rockets",
    "thunder": "thunder", "okc thunder": "thunder", "oklahoma city thunder": "thunder",
    "cavaliers": "cavaliers", "cavs": "cavaliers", "cleveland cavaliers": "cavaliers",
    "grizzlies": "grizzlies", "memphis grizzlies": "grizzlies", "memphis": "grizzlies",
    "timberwolves": "timberwolves", "wolves": "timberwolves",
    "minnesota timberwolves": "timberwolves",
    "pacers": "pacers", "indiana pacers": "pacers", "indiana": "pacers",
    "hawks": "hawks", "atlanta hawks": "hawks", "atlanta": "hawks",
    "bulls": "bulls", "chicago bulls": "bulls", "chicago": "bulls",
    "pistons": "pistons", "detroit pistons": "pistons", "detroit": "pistons",
    "magic": "magic", "orlando magic": "magic", "orlando": "magic",
    "hornets": "hornets", "charlotte hornets": "hornets", "charlotte": "hornets",
    "wizards": "wizards", "washington wizards": "wizards",
    "trail blazers": "blazers", "blazers": "blazers",
    "portland trail blazers": "blazers",
    "spurs": "spurs", "san antonio spurs": "spurs",
    "kings": "kings", "sacramento kings": "kings", "sacramento": "kings",
    "jazz": "jazz", "utah jazz": "jazz", "utah": "jazz",
    "pelicans": "pelicans", "new orleans pelicans": "pelicans",
    "raptors": "raptors", "toronto raptors": "raptors", "toronto": "raptors",
}


# ---------------------------------------------------------------------------
# Text processing
# ---------------------------------------------------------------------------

def _normalize_title(title: str) -> str:
    """Normalize a market title for comparison."""
    t = title.lower().strip()
    # Remove common punctuation
    t = re.sub(r"[?!.,;:\"'()\[\]{}]", " ", t)
    # Collapse whitespace
    t = re.sub(r"\s+", " ", t).strip()
    return t


def _extract_keywords(title: str) -> frozenset[str]:
    """Extract significant keywords from a market title."""
    normalized = _normalize_title(title)
    words = normalized.split()
    return frozenset(
        w for w in words
        if len(w) > 2 and w not in _STOP_WORDS
    )


def _extract_nba_teams(title: str) -> frozenset[str]:
    """Extract normalized NBA team names from a title."""
    t = title.lower()
    found: set[str] = set()
    for pattern, canonical in _NBA_TEAMS.items():
        if pattern in t:
            found.add(canonical)
    return frozenset(found)


def _extract_date_hint(title: str) -> str | None:
    """Extract a date-like pattern from a title (e.g., '3/26', 'March 26')."""
    # MM/DD pattern
    m = re.search(r"(\d{1,2})/(\d{1,2})", title)
    if m:
        return f"{int(m.group(1)):02d}/{int(m.group(2)):02d}"

    # Month Day pattern
    months = {
        "jan": "01", "feb": "02", "mar": "03", "apr": "04",
        "may": "05", "jun": "06", "jul": "07", "aug": "08",
        "sep": "09", "oct": "10", "nov": "11", "dec": "12",
    }
    for month_name, month_num in months.items():
        m = re.search(rf"{month_name}\w*\s+(\d{{1,2}})", title.lower())
        if m:
            return f"{month_num}/{int(m.group(1)):02d}"

    return None


# ---------------------------------------------------------------------------
# Matching
# ---------------------------------------------------------------------------

def _compute_similarity(poly_title: str, kalshi_title: str) -> float:
    """Compute similarity between two market titles.

    Uses a weighted combination of:
    - Token sort ratio (handles word reordering)
    - Partial ratio (handles substring matches)
    - NBA team matching bonus
    """
    poly_norm = _normalize_title(poly_title)
    kalshi_norm = _normalize_title(kalshi_title)

    # rapidfuzz scores are 0-100, normalize to 0-1
    token_sort = fuzz.token_sort_ratio(poly_norm, kalshi_norm) / 100.0
    partial = fuzz.partial_ratio(poly_norm, kalshi_norm) / 100.0

    # Weighted combination
    score = 0.6 * token_sort + 0.4 * partial

    # NBA team matching bonus
    poly_teams = _extract_nba_teams(poly_title)
    kalshi_teams = _extract_nba_teams(kalshi_title)
    if poly_teams and kalshi_teams:
        team_overlap = len(poly_teams & kalshi_teams)
        if team_overlap >= 2:
            score = max(score, 0.85)  # Strong match if both teams match
        elif team_overlap == 1:
            score += 0.1  # Partial bonus

    # Date matching bonus/penalty
    poly_date = _extract_date_hint(poly_title)
    kalshi_date = _extract_date_hint(kalshi_title)
    if poly_date and kalshi_date:
        if poly_date == kalshi_date:
            score += 0.05  # Same date = bonus
        else:
            score *= 0.5   # Different dates = strong penalty

    return min(score, 1.0)


def match_markets(
    poly_markets: Sequence[PolymarketMarket],
    kalshi_markets: Sequence[KalshiMarket],
    threshold: float = _MATCH_THRESHOLD,
) -> list[MatchedMarket]:
    """Find matching markets between Polymarket and Kalshi.

    Uses keyword pre-filtering to reduce the O(n*m) comparison space,
    then applies fuzzy matching on candidate pairs.

    Args:
        poly_markets: Markets from Polymarket.
        kalshi_markets: Markets from Kalshi.
        threshold: Minimum similarity score (0-1) for a match.

    Returns:
        List of MatchedMarket pairs, sorted by similarity descending.
    """
    if not poly_markets or not kalshi_markets:
        return []

    # Pre-compute keywords for all Kalshi markets
    kalshi_keywords = [_extract_keywords(km.title) for km in kalshi_markets]

    matches: list[MatchedMarket] = []
    used_kalshi: set[int] = set()

    for pm in poly_markets:
        pm_kw = _extract_keywords(pm.title)
        best_score = 0.0
        best_idx = -1

        for idx, km in enumerate(kalshi_markets):
            if idx in used_kalshi:
                continue

            # Cheap keyword overlap pre-filter
            shared = len(pm_kw & kalshi_keywords[idx])
            if shared < _KEYWORD_OVERLAP_MIN:
                continue

            # Full fuzzy similarity
            score = _compute_similarity(pm.title, km.title)
            if score > best_score:
                best_score = score
                best_idx = idx

        if best_score >= threshold and best_idx >= 0:
            used_kalshi.add(best_idx)
            matches.append(MatchedMarket(
                poly_market=pm,
                kalshi_market=kalshi_markets[best_idx],
                similarity=best_score,
            ))

    # Sort by similarity descending
    matches.sort(key=lambda m: m.similarity, reverse=True)
    return matches


# ---------------------------------------------------------------------------
# Arbitrage detection
# ---------------------------------------------------------------------------

def find_arbitrage(
    matches: Sequence[MatchedMarket],
    min_arb_pct: float = 1.0,
    poly_fee: float = 0.01,
    kalshi_fee: float = 0.038,
) -> list[ArbOpportunity]:
    """Calculate arbitrage opportunities from matched market pairs.

    An arb exists when you can buy complementary positions across exchanges
    for less than 100 cents total (after fees):

    Strategy A: Buy YES on Poly + Buy NO on Kalshi
      cost = poly_yes * (1 + poly_fee) + kalshi_no * (1 + kalshi_fee)
      arb  = 100 - cost

    Strategy B: Buy NO on Poly + Buy YES on Kalshi
      cost = poly_no * (1 + poly_fee) + kalshi_yes * (1 + kalshi_fee)
      arb  = 100 - cost

    Args:
        matches: Matched market pairs.
        min_arb_pct: Minimum arb percentage to include.
        poly_fee: Polymarket fee rate (default 1%).
        kalshi_fee: Kalshi fee rate (default 3.8%).

    Returns:
        List of ArbOpportunity objects, sorted by arb_pct descending.
    """
    opportunities: list[ArbOpportunity] = []

    for m in matches:
        pm = m.poly_market
        km = m.kalshi_market

        # Strategy A: YES on Poly + NO on Kalshi
        cost_a = pm.yes_price * (1 + poly_fee) + km.no_price * (1 + kalshi_fee)
        arb_a = 100 - cost_a

        # Strategy B: NO on Poly + YES on Kalshi
        cost_b = pm.no_price * (1 + poly_fee) + km.yes_price * (1 + kalshi_fee)
        arb_b = 100 - cost_b

        best_arb = max(arb_a, arb_b)

        if best_arb >= min_arb_pct:
            if arb_a >= arb_b:
                strategy = "Buy YES on Poly + Buy NO on Kalshi"
                arb_pct = arb_a
            else:
                strategy = "Buy NO on Poly + Buy YES on Kalshi"
                arb_pct = arb_b

            opportunities.append(ArbOpportunity(
                title=pm.title,
                poly_yes=pm.yes_price,
                poly_no=pm.no_price,
                kalshi_yes=km.yes_price,
                kalshi_no=km.no_price,
                arb_pct=round(arb_pct, 2),
                strategy=strategy,
                poly_url=pm.url,
                kalshi_url=km.url,
            ))

    # Best opportunities first
    opportunities.sort(key=lambda o: o.arb_pct, reverse=True)
    return opportunities
