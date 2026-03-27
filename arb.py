#!/usr/bin/env python3
"""Polymarket-Kalshi Arbitrage Scanner CLI.

Finds arbitrage opportunities between Polymarket and Kalshi prediction markets.

Usage:
    python arb.py scan                     # Scan for arb opportunities
    python arb.py scan --min-arb 2         # Only show arbs > 2%
    python arb.py scan --category politics  # Filter by category
    python arb.py markets                  # List all overlapping markets
    python arb.py watch                    # Continuous monitoring (30s refresh)
"""

from __future__ import annotations

import os
import sys
import time
import signal
from datetime import datetime, timezone
from pathlib import Path

import click
from dotenv import load_dotenv
from rich.console import Console
from rich.table import Table
from rich.panel import Panel
from rich.live import Live
from rich.text import Text
from rich.columns import Columns

from clients.polymarket import PolymarketClient
from clients.kalshi import KalshiClient
from matcher import match_markets, find_arbitrage, ArbOpportunity, MatchedMarket

# Load .env from the project directory
load_dotenv(Path(__file__).resolve().parent / ".env")

console = Console()

# ---------------------------------------------------------------------------
# Fee constants
# ---------------------------------------------------------------------------
POLYMARKET_FEE = 0.01    # ~1%
KALSHI_FEE = 0.038       # ~3.8%


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _fetch_all_markets(
    category: str | None = None,
    poly_pages: int = 3,
    kalshi_pages: int = 5,
) -> tuple[list, list]:
    """Fetch markets from both exchanges with progress indication."""
    poly_client = PolymarketClient(
        api_key=os.getenv("POLYMARKET_API_KEY", ""),
    )
    kalshi_client = KalshiClient(
        email=os.getenv("KALSHI_API_EMAIL", ""),
        password=os.getenv("KALSHI_API_PASSWORD", ""),
    )

    with console.status("[bold cyan]Fetching Polymarket markets..."):
        poly_markets = poly_client.fetch_markets(
            max_pages=poly_pages,
            category=category,
        )

    with console.status("[bold cyan]Fetching Kalshi markets..."):
        kalshi_markets = kalshi_client.fetch_markets(
            max_pages=kalshi_pages,
            category=category,
        )

    return poly_markets, kalshi_markets


def _build_arb_table(
    opportunities: list[ArbOpportunity],
    title: str = "Arbitrage Opportunities",
) -> Table:
    """Build a rich table of arbitrage opportunities."""
    table = Table(
        title=title,
        title_style="bold magenta",
        border_style="bright_blue",
        show_lines=True,
        pad_edge=True,
    )

    table.add_column("#", style="dim", width=4, justify="right")
    table.add_column("Market", style="white", max_width=45, no_wrap=False)
    table.add_column("Poly YES", style="green", justify="center", width=10)
    table.add_column("Kalshi YES", style="yellow", justify="center", width=11)
    table.add_column("Arb %", justify="center", width=8)
    table.add_column("Strategy", style="dim cyan", max_width=38, no_wrap=False)
    table.add_column("Max Bet", justify="right", width=9)

    for i, opp in enumerate(opportunities, 1):
        # Color the arb percentage based on magnitude
        if opp.arb_pct >= 5.0:
            arb_style = "bold bright_green"
        elif opp.arb_pct >= 3.0:
            arb_style = "bold green"
        elif opp.arb_pct >= 1.0:
            arb_style = "green"
        else:
            arb_style = "dim green"

        arb_text = Text(f"+{opp.arb_pct:.1f}%", style=arb_style)

        # Estimate max bet size (conservative: $1000 per side, limited by arb margin)
        max_bet = f"${min(1000, int(1000 * (opp.arb_pct / 100) * 10)):,}"

        table.add_row(
            str(i),
            opp.title[:45],
            f"{opp.poly_yes:.0f}c",
            f"{opp.kalshi_yes:.0f}c",
            arb_text,
            opp.strategy,
            max_bet,
        )

    return table


def _build_markets_table(
    matches: list[MatchedMarket],
    title: str = "Overlapping Markets",
) -> Table:
    """Build a rich table of matched markets across both exchanges."""
    table = Table(
        title=title,
        title_style="bold cyan",
        border_style="bright_blue",
        show_lines=True,
    )

    table.add_column("#", style="dim", width=4, justify="right")
    table.add_column("Polymarket", style="green", max_width=40, no_wrap=False)
    table.add_column("Kalshi", style="yellow", max_width=40, no_wrap=False)
    table.add_column("Similarity", justify="center", width=11)
    table.add_column("Poly YES", justify="center", width=10)
    table.add_column("Kalshi YES", justify="center", width=11)
    table.add_column("Spread", justify="center", width=8)

    for i, m in enumerate(matches, 1):
        sim_pct = f"{m.similarity * 100:.0f}%"
        if m.similarity >= 0.8:
            sim_style = "bold green"
        elif m.similarity >= 0.6:
            sim_style = "yellow"
        else:
            sim_style = "dim red"

        spread = abs(m.poly_market.yes_price - m.kalshi_market.yes_price)
        spread_text = f"{spread:.1f}c"

        table.add_row(
            str(i),
            m.poly_market.title[:40],
            m.kalshi_market.title[:40],
            Text(sim_pct, style=sim_style),
            f"{m.poly_market.yes_price:.0f}c",
            f"{m.kalshi_market.yes_price:.0f}c",
            spread_text,
        )

    return table


def _build_summary_panel(
    poly_count: int,
    kalshi_count: int,
    match_count: int,
    arb_count: int,
    timestamp: datetime | None = None,
) -> Panel:
    """Build a summary statistics panel."""
    ts = timestamp or datetime.now(timezone.utc)
    ts_str = ts.strftime("%Y-%m-%d %H:%M:%S UTC")

    stats = (
        f"[bold]Scan Time:[/bold] {ts_str}\n"
        f"[bold]Polymarket:[/bold] {poly_count} markets  "
        f"[bold]Kalshi:[/bold] {kalshi_count} markets\n"
        f"[bold]Matched:[/bold] {match_count} pairs  "
        f"[bold]Arb Opportunities:[/bold] [{'green' if arb_count > 0 else 'red'}]{arb_count}[/]\n"
        f"[bold]Fees:[/bold] Polymarket ~{POLYMARKET_FEE*100:.0f}%  "
        f"Kalshi ~{KALSHI_FEE*100:.1f}%"
    )

    return Panel(
        stats,
        title="[bold bright_blue]Polymarket-Kalshi Arb Scanner[/bold bright_blue]",
        border_style="bright_blue",
        padding=(1, 2),
    )


# ---------------------------------------------------------------------------
# CLI Commands
# ---------------------------------------------------------------------------

@click.group()
@click.version_option(version="1.0.0", prog_name="polymarket-arb")
def cli():
    """Polymarket-Kalshi Arbitrage Scanner.

    Find arbitrage opportunities between Polymarket and Kalshi prediction markets.
    """
    pass


@cli.command()
@click.option(
    "--min-arb",
    type=float,
    default=1.0,
    show_default=True,
    help="Minimum arb percentage to display.",
)
@click.option(
    "--category",
    type=str,
    default=None,
    help="Filter by category (e.g., nba, politics, crypto).",
)
@click.option(
    "--poly-pages",
    type=int,
    default=3,
    show_default=True,
    help="Number of Polymarket API pages to fetch.",
)
@click.option(
    "--kalshi-pages",
    type=int,
    default=5,
    show_default=True,
    help="Number of Kalshi API pages to fetch.",
)
@click.option(
    "--show-matches",
    is_flag=True,
    default=False,
    help="Also show all matched market pairs (not just arbs).",
)
def scan(
    min_arb: float,
    category: str | None,
    poly_pages: int,
    kalshi_pages: int,
    show_matches: bool,
):
    """Scan for arbitrage opportunities across both exchanges."""
    try:
        poly_markets, kalshi_markets = _fetch_all_markets(
            category=category,
            poly_pages=poly_pages,
            kalshi_pages=kalshi_pages,
        )
    except Exception as exc:
        console.print(f"[bold red]Error fetching markets:[/bold red] {exc}")
        sys.exit(1)

    if not poly_markets and not kalshi_markets:
        console.print("[yellow]No markets fetched from either exchange. Check API keys / connectivity.[/yellow]")
        sys.exit(1)

    with console.status("[bold cyan]Matching markets across exchanges..."):
        matches = match_markets(poly_markets, kalshi_markets)

    with console.status("[bold cyan]Calculating arbitrage opportunities..."):
        opportunities = find_arbitrage(
            matches,
            min_arb_pct=min_arb,
            poly_fee=POLYMARKET_FEE,
            kalshi_fee=KALSHI_FEE,
        )

    # Summary
    ts = datetime.now(timezone.utc)
    summary = _build_summary_panel(
        poly_count=len(poly_markets),
        kalshi_count=len(kalshi_markets),
        match_count=len(matches),
        arb_count=len(opportunities),
        timestamp=ts,
    )
    console.print(summary)

    # Matched markets table
    if show_matches and matches:
        console.print()
        console.print(_build_markets_table(matches))

    # Arbitrage table
    if opportunities:
        console.print()
        console.print(_build_arb_table(
            opportunities,
            title=f"Arbitrage Opportunities (>= {min_arb:.1f}%)",
        ))
        console.print()
        console.print(
            f"[dim]Tip: Arb % already accounts for fees "
            f"(Poly {POLYMARKET_FEE*100:.0f}% + Kalshi {KALSHI_FEE*100:.1f}%).[/dim]"
        )
    else:
        console.print()
        console.print(
            f"[yellow]No arb opportunities >= {min_arb:.1f}% found.[/yellow]"
        )
        if not matches:
            console.print(
                "[dim]No markets matched between exchanges. "
                "Try fetching more pages or broadening the category filter.[/dim]"
            )


@cli.command()
@click.option(
    "--category",
    type=str,
    default=None,
    help="Filter by category (e.g., nba, politics, crypto).",
)
@click.option(
    "--poly-pages",
    type=int,
    default=3,
    show_default=True,
    help="Number of Polymarket API pages to fetch.",
)
@click.option(
    "--kalshi-pages",
    type=int,
    default=5,
    show_default=True,
    help="Number of Kalshi API pages to fetch.",
)
def markets(
    category: str | None,
    poly_pages: int,
    kalshi_pages: int,
):
    """List all markets available on both exchanges (matched pairs)."""
    try:
        poly_markets, kalshi_markets = _fetch_all_markets(
            category=category,
            poly_pages=poly_pages,
            kalshi_pages=kalshi_pages,
        )
    except Exception as exc:
        console.print(f"[bold red]Error fetching markets:[/bold red] {exc}")
        sys.exit(1)

    with console.status("[bold cyan]Matching markets across exchanges..."):
        matches = match_markets(poly_markets, kalshi_markets)

    # Summary
    summary = _build_summary_panel(
        poly_count=len(poly_markets),
        kalshi_count=len(kalshi_markets),
        match_count=len(matches),
        arb_count=0,
    )
    console.print(summary)

    if matches:
        console.print()
        console.print(_build_markets_table(matches))
    else:
        console.print()
        console.print("[yellow]No overlapping markets found between exchanges.[/yellow]")

    # Also show counts of unmatched markets
    console.print()
    console.print(
        f"[dim]Unmatched: {len(poly_markets) - len(matches)} Polymarket, "
        f"{len(kalshi_markets) - len(matches)} Kalshi[/dim]"
    )


@cli.command()
@click.option(
    "--min-arb",
    type=float,
    default=1.0,
    show_default=True,
    help="Minimum arb percentage to display.",
)
@click.option(
    "--category",
    type=str,
    default=None,
    help="Filter by category (e.g., nba, politics, crypto).",
)
@click.option(
    "--interval",
    type=int,
    default=30,
    show_default=True,
    help="Refresh interval in seconds.",
)
@click.option(
    "--poly-pages",
    type=int,
    default=3,
    show_default=True,
    help="Number of Polymarket API pages to fetch.",
)
@click.option(
    "--kalshi-pages",
    type=int,
    default=5,
    show_default=True,
    help="Number of Kalshi API pages to fetch.",
)
def watch(
    min_arb: float,
    category: str | None,
    interval: int,
    poly_pages: int,
    kalshi_pages: int,
):
    """Continuously monitor for arbitrage opportunities (refreshes every N seconds)."""
    # Cache for last fetched data
    cached_poly = []
    cached_kalshi = []
    cached_matches = []
    cached_arbs = []
    last_fetch = None
    scan_count = 0

    def _do_scan():
        nonlocal cached_poly, cached_kalshi, cached_matches, cached_arbs
        nonlocal last_fetch, scan_count

        scan_count += 1
        try:
            poly_markets, kalshi_markets = _fetch_all_markets(
                category=category,
                poly_pages=poly_pages,
                kalshi_pages=kalshi_pages,
            )
            cached_poly = poly_markets
            cached_kalshi = kalshi_markets
        except Exception as exc:
            console.print(f"[bold red]Fetch error (scan #{scan_count}):[/bold red] {exc}")
            return

        cached_matches = match_markets(cached_poly, cached_kalshi)
        cached_arbs = find_arbitrage(
            cached_matches,
            min_arb_pct=min_arb,
            poly_fee=POLYMARKET_FEE,
            kalshi_fee=KALSHI_FEE,
        )
        last_fetch = datetime.now(timezone.utc)

    def _render():
        """Generate the full display for one refresh cycle."""
        parts = []

        ts = last_fetch or datetime.now(timezone.utc)
        summary = _build_summary_panel(
            poly_count=len(cached_poly),
            kalshi_count=len(cached_kalshi),
            match_count=len(cached_matches),
            arb_count=len(cached_arbs),
            timestamp=ts,
        )
        parts.append(summary)

        if cached_arbs:
            parts.append(
                _build_arb_table(
                    cached_arbs,
                    title=f"Live Arb Monitor (>= {min_arb:.1f}%) — Scan #{scan_count}",
                )
            )
        else:
            parts.append(
                Text(
                    f"No arb opportunities >= {min_arb:.1f}% — "
                    f"refreshing in {interval}s...",
                    style="yellow",
                )
            )

        parts.append(
            Text(
                f"\nPress Ctrl+C to stop. Next refresh in {interval}s.",
                style="dim",
            )
        )

        return Columns(parts, expand=True) if len(parts) == 1 else parts

    console.print(
        f"[bold bright_blue]Starting watch mode[/bold bright_blue] "
        f"(interval={interval}s, min_arb={min_arb}%)"
    )
    console.print("[dim]Press Ctrl+C to stop.[/dim]\n")

    # Handle SIGINT gracefully
    stop = False

    def _sigint_handler(sig, frame):
        nonlocal stop
        stop = True

    signal.signal(signal.SIGINT, _sigint_handler)

    try:
        while not stop:
            _do_scan()

            # Clear and render
            console.clear()
            summary = _build_summary_panel(
                poly_count=len(cached_poly),
                kalshi_count=len(cached_kalshi),
                match_count=len(cached_matches),
                arb_count=len(cached_arbs),
                timestamp=last_fetch,
            )
            console.print(summary)

            if cached_arbs:
                console.print()
                console.print(
                    _build_arb_table(
                        cached_arbs,
                        title=f"Live Arb Monitor (>= {min_arb:.1f}%) — Scan #{scan_count}",
                    )
                )
            else:
                console.print()
                console.print(
                    f"[yellow]No arb opportunities >= {min_arb:.1f}% found.[/yellow]"
                )

            console.print()
            console.print(
                f"[dim]Scan #{scan_count} complete. "
                f"Next refresh in {interval}s. Ctrl+C to stop.[/dim]"
            )

            # Sleep in small increments so Ctrl+C is responsive
            for _ in range(interval * 2):
                if stop:
                    break
                time.sleep(0.5)

    except KeyboardInterrupt:
        pass

    console.print("\n[bold bright_blue]Watch mode stopped.[/bold bright_blue]")


if __name__ == "__main__":
    cli()
