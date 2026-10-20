#!/usr/bin/env python3
"""Read-only Yahoo GC=F gold futures proxy; NOT XAUUSD spot. Stdlib only."""
import json
import math
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

BASE = "https://query1.finance.yahoo.com/v8/finance/chart/GC%3DF"
# Yahoo limits intraday history. Ranges also provide room for market closures.
RANGES = {"1m": "5d", "5m": "1mo", "15m": "1mo", "30m": "1mo",
          "1h": "1mo", "1d": "2y", "1wk": "5y", "1mo": "10y"}
MAX_RESPONSE = 4 * 1024 * 1024


def number(value):
    if isinstance(value, bool):
        return None
    if isinstance(value, (int, float)) and math.isfinite(value):
        return value
    return None


def fetch(interval):
    url = BASE + "?" + urllib.parse.urlencode({
        "interval": interval, "range": RANGES[interval], "includePrePost": "false"})
    req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0 dani-ai/1"})
    with urllib.request.urlopen(req, timeout=15) as response:
        data = response.read(MAX_RESPONSE + 1)
    if len(data) > MAX_RESPONSE:
        raise ValueError("Yahoo response exceeds size limit")
    return json.loads(data)


def parse_chart(payload, interval, limit, now=None):
    now = int(time.time()) if now is None else now
    chart = payload["chart"]
    if chart.get("error"):
        raise ValueError("Yahoo chart API returned an error")
    result = chart["result"][0]
    meta = result["meta"]
    if meta.get("symbol") != "GC=F" or meta.get("currency") != "USD":
        raise ValueError("unexpected instrument or currency from Yahoo")
    timestamps = result.get("timestamp") or []
    quote = result["indicators"]["quote"][0]
    candles = []
    for i, timestamp in enumerate(timestamps):
        if number(timestamp) is None:
            continue
        row = {"t": timestamp}
        for source, target in (("open", "o"), ("high", "h"), ("low", "l"),
                               ("close", "c"), ("volume", "v")):
            values = quote.get(source) or []
            row[target] = number(values[i]) if i < len(values) else None
        if any(row[k] is None for k in ("o", "h", "l", "c")):
            continue
        candles.append(row)
    candles = candles[-limit:]
    if not candles:
        raise ValueError("Yahoo returned no usable OHLC candles")
    quote_time = number(meta.get("regularMarketTime"))
    previous = number(meta.get("chartPreviousClose"))
    last = number(meta.get("regularMarketPrice"))
    return {
        "requested_instrument": "XAUUSD", "symbol": "GC=F",
        "instrument": "COMEX gold futures proxy, NOT XAUUSD spot",
        "source": "yahoo-finance-chart", "currency": "USD",
        "price_unit": "USD per troy ounce", "fetched_at": now,
        "timestamp_unit": "unix seconds", "exchange_timezone": meta.get("exchangeTimezoneName"),
        "warnings": ["GC=F is a gold futures proxy, not a spot XAUUSD quote; futures basis and rolls can affect prices.",
                     "Yahoo data may be delayed. Market closures also increase quote age; not an execution feed.",
                     "The latest candle may be incomplete. Volume is futures volume, not spot volume.",
                     "Yahoo is an unofficial public endpoint and may throttle or block requests."],
        "quote": {"last": last, "time": quote_time,
                  "age_seconds": max(0, now - quote_time) if quote_time is not None else None,
                  "chart_previous_close": previous},
        "klines": {"interval": interval, "count": len(candles), "candles": candles},
        "window": {"first_close": candles[0]["c"], "last_close": candles[-1]["c"],
                   "high": max(c["h"] for c in candles), "low": min(c["l"] for c in candles)},
    }


def main():
    try:
        raw = sys.stdin.read(65537)
        if len(raw) > 65536:
            raise ValueError("arguments too large")
        args = json.loads(raw)
        if not isinstance(args, dict) or set(args) - {"interval", "limit"}:
            raise ValueError("arguments must be an object with only interval and limit")
        interval = args.get("interval", "1h")
        limit = args.get("limit", 60)
        if not isinstance(interval, str) or interval not in RANGES:
            raise ValueError("unsupported interval")
        if isinstance(limit, bool) or not isinstance(limit, int) or not 1 <= limit <= 200:
            raise ValueError("limit must be an integer 1..200")
        result = parse_chart(fetch(interval), interval, limit)
        print(json.dumps(result, ensure_ascii=False, allow_nan=False))
        return 0
    except urllib.error.HTTPError as e:
        e.close()
        print(json.dumps({"error": "Yahoo HTTP %s; data unavailable (possibly throttled)" % e.code}))
    except (OSError, ValueError, KeyError, IndexError, TypeError, AttributeError) as e:
        print(json.dumps({"error": "gold futures data unavailable: %s" % str(e)[:200]}))
    return 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except BrokenPipeError:
        sys.exit(1)
