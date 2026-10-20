#!/usr/bin/env python3
"""btc_market — dani-ai custom tool: public Binance USD-M futures data.

Protocol: read one JSON object from stdin, print one JSON object to stdout.
Read-only market data (no API key, no orders). Stdlib only.
"""
import json
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

BASE = "https://fapi.binance.com"
SECTIONS = ("ticker", "klines", "funding", "open_interest", "long_short")
INTERVALS = {"1m", "5m", "15m", "30m", "1h", "2h", "4h", "6h", "8h", "12h", "1d", "1w"}
TIMEOUT = 10
# Account-ratio endpoint periods differ from kline intervals. Use the nearest
# supported period at or below the requested interval.
LONG_SHORT_PERIODS = {"1m": "5m", "8h": "6h", "1w": "1d"}


def get(path, params):
    url = BASE + path + "?" + urllib.parse.urlencode(params)
    request = urllib.request.Request(url, headers={"User-Agent": "dani-ai-tool/1"})
    with urllib.request.urlopen(request, timeout=TIMEOUT) as response:
        return json.loads(response.read().decode("utf-8"))


def number(value):
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def main():
    try:
        args = json.load(sys.stdin)
    except ValueError as e:
        print(json.dumps({"error": "invalid JSON arguments: %s" % e}))
        return 2
    if not isinstance(args, dict):
        print(json.dumps({"error": "arguments must be an object"}))
        return 2
    symbol = args.get("symbol", "BTCUSDT")
    interval = args.get("interval", "1h")
    limit = args.get("limit", 60)
    include = args.get("include") or list(SECTIONS)
    if not isinstance(symbol, str) or not symbol.isalnum() or not 3 <= len(symbol) <= 24:
        print(json.dumps({"error": "symbol must be alphanumeric (e.g. BTCUSDT)"}))
        return 2
    symbol = symbol.upper()
    if interval not in INTERVALS:
        print(json.dumps({"error": "unsupported interval", "allowed": sorted(INTERVALS)}))
        return 2
    if not isinstance(limit, int) or isinstance(limit, bool) or not 1 <= limit <= 200:
        print(json.dumps({"error": "limit must be an integer 1..200"}))
        return 2
    if not isinstance(include, list) or not include or any(s not in SECTIONS for s in include):
        print(json.dumps({"error": "include must be a non-empty subset of %s" % list(SECTIONS)}))
        return 2

    result = {"symbol": symbol, "source": "binance-usdm-public",
              "fetched_at": int(time.time() * 1000), "errors": []}
    for section in include:
        try:
            if section == "ticker":
                raw = get("/fapi/v1/ticker/24hr", {"symbol": symbol})
                result["ticker"] = {
                    "last": number(raw.get("lastPrice")),
                    "open_24h": number(raw.get("openPrice")),
                    "high_24h": number(raw.get("highPrice")),
                    "low_24h": number(raw.get("lowPrice")),
                    "change_24h": number(raw.get("priceChange")),
                    "change_pct_24h": number(raw.get("priceChangePercent")),
                    "quote_volume_24h": number(raw.get("quoteVolume")),
                    "trades_24h": raw.get("count"),
                }
            elif section == "klines":
                rows = get("/fapi/v1/klines", {"symbol": symbol, "interval": interval,
                                               "limit": limit})
                candles = []
                for row in rows:
                    if not isinstance(row, list) or len(row) < 6:
                        continue
                    candles.append({
                        "t": row[0], "o": number(row[1]), "h": number(row[2]),
                        "l": number(row[3]), "c": number(row[4]),
                        "v": number(row[5]), "qv": number(row[7]) if len(row) > 7 else None,
                    })
                result["klines"] = {"interval": interval, "count": len(candles),
                                    "candles": candles}
                closes = [c["c"] for c in candles if c["c"] is not None]
                highs = [c["h"] for c in candles if c["h"] is not None]
                lows = [c["l"] for c in candles if c["l"] is not None]
                if closes:
                    result["window"] = {
                        "bars": len(candles),
                        "first_close": closes[0],
                        "last_close": closes[-1],
                        "high": max(highs) if highs else None,
                        "low": min(lows) if lows else None,
                        "change_pct": round((closes[-1] / closes[0] - 1) * 100, 4)
                        if closes[0] else None,
                    }
            elif section == "funding":
                raw = get("/fapi/v1/premiumIndex", {"symbol": symbol})
                rate = number(raw.get("lastFundingRate"))
                result["funding"] = {
                    "mark_price": number(raw.get("markPrice")),
                    "index_price": number(raw.get("indexPrice")),
                    "last_funding_rate": rate,
                    "last_funding_rate_pct": round(rate * 100, 6) if rate is not None else None,
                    "annualized_pct": round(rate * 3 * 365 * 100, 3) if rate is not None else None,
                    "next_funding_time": raw.get("nextFundingTime"),
                }
            elif section == "open_interest":
                raw = get("/fapi/v1/openInterest", {"symbol": symbol})
                result["open_interest"] = {"contracts": number(raw.get("openInterest")),
                                           "time": raw.get("time")}
            elif section == "long_short":
                period = LONG_SHORT_PERIODS.get(interval, interval)
                raw = get("/futures/data/globalLongShortAccountRatio",
                          {"symbol": symbol, "period": period,
                           "limit": min(limit, 30)})
                rows = raw if isinstance(raw, list) else []
                long = number(rows[-1].get("longAccount")) if rows else None
                short = number(rows[-1].get("shortAccount")) if rows else None
                result["long_short"] = {
                    "period": period,
                    "latest": {
                        "long_pct": long * 100 if long is not None else None,
                        "short_pct": short * 100 if short is not None else None,
                        "ratio": number(rows[-1].get("longShortRatio")) if rows else None,
                        "time": rows[-1].get("timestamp") if rows else None,
                    },
                    "samples": len(rows),
                }
        except urllib.error.HTTPError as e:
            result["errors"].append({"section": section, "http": e.code,
                                     "message": "request rejected by Binance"})
        except (urllib.error.URLError, OSError, ValueError) as e:
            result["errors"].append({"section": section, "message": str(e)[:200]})
    print(json.dumps(result, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except BrokenPipeError:
        sys.exit(1)
