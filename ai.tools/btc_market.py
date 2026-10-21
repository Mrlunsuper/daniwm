#!/usr/bin/env python3
"""btc_market — dani-ai custom tool: public Binance USD-M futures data.

Protocol: read one JSON object from stdin, print one JSON object to stdout.
Read-only market data (no API key, no orders). Stdlib only.
"""
import json
import math
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

BASE = "https://fapi.binance.com"
SECTIONS = ("ticker", "klines", "funding", "open_interest", "long_short", "open_interest_history")
DEFAULT_SECTIONS = SECTIONS[:-1]  # history and multi-timeframe requests are opt-in
INTERVALS = {"1m", "5m", "15m", "30m", "1h", "2h", "4h", "6h", "8h", "12h", "1d", "1w"}
TIMEOUT = 10
TOTAL_TIMEOUT = 24  # leave room before the manifest's 30s process deadline
MAX_RESPONSE = 2 * 1024 * 1024
# Account-ratio endpoint periods differ from kline intervals. Use the nearest
# supported period at or below the requested interval.
LONG_SHORT_PERIODS = {"1m": "5m", "8h": "6h", "1w": "1d"}


def get(path, params, deadline=None):
    url = BASE + path + "?" + urllib.parse.urlencode(params)
    request = urllib.request.Request(url, headers={"User-Agent": "dani-ai-tool/1"})
    deadline = time.monotonic() + TIMEOUT if deadline is None else deadline
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        raise TimeoutError("market-data budget exhausted")
    with urllib.request.urlopen(request, timeout=min(TIMEOUT, remaining)) as response:
        data = bytearray()
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("market-data budget exhausted")
            sock = getattr(getattr(getattr(response, "fp", None), "raw", None), "_sock", None)
            if sock is not None:
                sock.settimeout(min(TIMEOUT, remaining))
            chunk = response.read1(min(65536, MAX_RESPONSE + 1 - len(data)))
            if not chunk:
                break
            data.extend(chunk)
            if len(data) > MAX_RESPONSE:
                raise ValueError("Binance response exceeds size limit")
        if time.monotonic() >= deadline:
            raise TimeoutError("market-data budget exhausted")
        return json.loads(data)


def number(value):
    if isinstance(value, bool):
        return None
    try:
        result = float(value)
        return result if math.isfinite(result) else None
    except (TypeError, ValueError, OverflowError):
        return None


def change_pct(first, last):
    if first is None or last is None or first <= 0:
        return None
    return number(round((last / first - 1) * 100, 4))


def candle_data(rows):
    candles = []
    now = int(time.time() * 1000)
    for row in rows:
        if not isinstance(row, list) or len(row) < 6:
            continue
        if number(row[0]) is None or any(number(row[i]) is None for i in (1, 2, 3, 4)):
            continue
        close_time = number(row[6]) if len(row) > 6 else None
        candles.append({
            "close_time": close_time,
            "is_closed": close_time < now if close_time is not None else None,
            "t": number(row[0]), "o": number(row[1]), "h": number(row[2]),
            "l": number(row[3]), "c": number(row[4]), "v": number(row[5]),
            "qv": number(row[7]) if len(row) > 7 else None,
        })
    return candles


def window_stats(candles):
    if not candles:
        return None
    return {"bars": len(candles), "first_close": candles[0]["c"],
            "last_close": candles[-1]["c"],
            "high": max(c["h"] for c in candles),
            "low": min(c["l"] for c in candles),
            "change_pct": change_pct(candles[0]["c"], candles[-1]["c"])}


def main():
    try:
        text = sys.stdin.read(65537)
        if len(text) > 65536:
            raise ValueError("arguments exceed size limit")
        args = json.loads(text)
    except ValueError as e:
        print(json.dumps({"error": "invalid JSON arguments: %s" % e}))
        return 2
    if not isinstance(args, dict) or set(args) - {"symbol", "interval", "intervals", "limit", "include"}:
        print(json.dumps({"error": "arguments must be an object"}))
        return 2
    symbol = args.get("symbol", "BTCUSDT")
    interval = args.get("interval", "1h")
    limit = args.get("limit", 60)
    include = args.get("include", list(DEFAULT_SECTIONS))
    intervals = args.get("intervals", [interval])
    if not isinstance(symbol, str) or not symbol.isalnum() or not 3 <= len(symbol) <= 24:
        print(json.dumps({"error": "symbol must be alphanumeric (e.g. BTCUSDT)"}))
        return 2
    symbol = symbol.upper()
    if not isinstance(interval, str) or interval not in INTERVALS:
        print(json.dumps({"error": "unsupported interval", "allowed": sorted(INTERVALS)}))
        return 2
    if not isinstance(limit, int) or isinstance(limit, bool) or not 1 <= limit <= 200:
        print(json.dumps({"error": "limit must be an integer 1..200"}))
        return 2
    if (not isinstance(intervals, list) or not 1 <= len(intervals) <= 4
            or any(not isinstance(i, str) or i not in INTERVALS for i in intervals)
            or len(set(intervals)) != len(intervals) or interval not in intervals
            or len(intervals) * limit > 400):
        print(json.dumps({"error": "intervals must contain interval, 1..4 unique supported frames; total candles <= 400"}))
        return 2
    if not isinstance(include, list) or not include or len(include) > len(SECTIONS) or any(s not in SECTIONS for s in include):
        print(json.dumps({"error": "include must be a non-empty subset of %s" % list(SECTIONS)}))
        return 2

    now = int(time.time() * 1000)
    result = {"symbol": symbol, "source": "binance-usdm-public",
              "fetched_at": now, "timestamp_unit": "unix milliseconds",
              "warnings": ["Latest candle may be incomplete; snapshot OI alone is not an OI trend.",
                           "Historical OI is sampled (last month only); changes are descriptive, not trade signals.",
                           "Annualized funding assumes 8-hour intervals and a constant rate; not a forecast."],
              "errors": []}
    deadline = time.monotonic() + TOTAL_TIMEOUT
    include = list(dict.fromkeys(include))

    def fetch(path, params, expected):
        if time.monotonic() >= deadline:
            raise TimeoutError("market-data budget exhausted")
        raw = get(path, params, deadline=deadline)
        if not isinstance(raw, expected):
            raise ValueError("unexpected Binance response shape")
        return raw

    jobs = [(section, frame) for section in include
            for frame in (intervals if section == "klines" else [interval])]
    for section, frame in jobs:
        try:
            if section == "ticker":
                raw = fetch("/fapi/v1/ticker/24hr", {"symbol": symbol}, dict)
                result["ticker"] = {
                    "last": number(raw.get("lastPrice")),
                    "open_24h": number(raw.get("openPrice")),
                    "high_24h": number(raw.get("highPrice")),
                    "low_24h": number(raw.get("lowPrice")),
                    "change_24h": number(raw.get("priceChange")),
                    "change_pct_24h": number(raw.get("priceChangePercent")),
                    "quote_volume_24h": number(raw.get("quoteVolume")),
                    "trades_24h": number(raw.get("count")),
                    "time": number(raw.get("closeTime")),
                }
            elif section == "klines":
                rows = fetch("/fapi/v1/klines", {"symbol": symbol, "interval": frame,
                                                 "limit": limit}, list)
                candles = candle_data(rows[:limit])
                data = {"interval": frame, "count": len(candles), "candles": candles,
                        "window": window_stats(candles),
                        "closed_window": window_stats([c for c in candles if c["is_closed"]])}
                if len(intervals) > 1:
                    result.setdefault("timeframes", {})[frame] = data
                if frame == interval:
                    result["klines"] = data
                    if data["window"] is not None:
                        result["window"] = data["window"]
            elif section == "funding":
                raw = fetch("/fapi/v1/premiumIndex", {"symbol": symbol}, dict)
                rate = number(raw.get("lastFundingRate"))
                if rate is not None and abs(rate) > 1:
                    raise ValueError("funding rate outside valid fractional range")
                result["funding"] = {
                    "mark_price": number(raw.get("markPrice")),
                    "index_price": number(raw.get("indexPrice")),
                    "last_funding_rate": rate,
                    "last_funding_rate_pct": round(rate * 100, 6) if rate is not None else None,
                    "annualized_pct": round(rate * 3 * 365 * 100, 3) if rate is not None and math.isfinite(rate * 109500) else None,
                    "annualized_assumed_interval_hours": 8,
                    "time": number(raw.get("time")),
                    "next_funding_time": number(raw.get("nextFundingTime")),
                }
            elif section == "open_interest":
                raw = fetch("/fapi/v1/openInterest", {"symbol": symbol}, dict)
                result["open_interest"] = {"contracts": number(raw.get("openInterest")),
                                           "time": number(raw.get("time"))}
            elif section == "open_interest_history":
                period = LONG_SHORT_PERIODS.get(interval, interval)
                raw = fetch("/futures/data/openInterestHist",
                            {"symbol": symbol, "period": period, "limit": limit}, list)
                samples = []
                for row in raw[:limit]:
                    if not isinstance(row, dict):
                        raise ValueError("unexpected OI history row shape")
                    timestamp = number(row.get("timestamp"))
                    contracts = number(row.get("sumOpenInterest"))
                    value = number(row.get("sumOpenInterestValue"))
                    if (timestamp is None or timestamp <= 0 or contracts is None or contracts < 0
                            or value is None or value < 0):
                        raise ValueError("invalid OI history sample")
                    if samples and timestamp <= samples[-1]["time"]:
                        raise ValueError("OI history timestamps must increase")
                    samples.append({"time": timestamp, "contracts": contracts, "value": value})
                result[section] = {
                    "period": period, "count": len(samples), "samples": samples,
                    "contracts_change_pct": change_pct(samples[0]["contracts"], samples[-1]["contracts"]) if len(samples) >= 2 else None,
                    "value_change_pct": change_pct(samples[0]["value"], samples[-1]["value"]) if len(samples) >= 2 else None,
                    "age_seconds": max(0, (time.time() * 1000 - samples[-1]["time"]) / 1000) if samples else None,
                }
            elif section == "long_short":
                period = LONG_SHORT_PERIODS.get(interval, interval)
                raw = fetch("/futures/data/globalLongShortAccountRatio",
                          {"symbol": symbol, "period": period,
                           "limit": min(limit, 30)}, list)
                if any(not isinstance(row, dict) for row in raw):
                    raise ValueError("unexpected ratio row shape")
                rows = raw
                long = number(rows[-1].get("longAccount")) if rows else None
                short = number(rows[-1].get("shortAccount")) if rows else None
                if any(value is not None and not 0 <= value <= 1 for value in (long, short)):
                    raise ValueError("account fraction outside 0..1")
                result["long_short"] = {
                    "period": period,
                    "latest": {
                        "long_pct": long * 100 if long is not None else None,
                        "short_pct": short * 100 if short is not None else None,
                        "ratio": number(rows[-1].get("longShortRatio")) if rows else None,
                        "time": number(rows[-1].get("timestamp")) if rows else None,
                    },
                    "samples": len(rows),
                }
        except urllib.error.HTTPError as e:
            e.close()
            result["errors"].append({"section": section, "interval": frame, "http": e.code,
                                     "message": "request rejected by Binance"})
        except (urllib.error.URLError, OSError, ValueError, TypeError, KeyError, IndexError) as e:
            result["errors"].append({"section": section, "interval": frame, "message": str(e)[:200]})
    result["partial"] = bool(result["errors"])
    for key in ("ticker", "funding", "open_interest"):
        if key in result:
            timestamp = result[key].get("time")
            result[key]["age_seconds"] = max(0, (time.time() * 1000 - timestamp) / 1000) if timestamp is not None else None
    result["completed_at"] = int(time.time() * 1000)
    print(json.dumps(result, ensure_ascii=False, allow_nan=False))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except BrokenPipeError:
        sys.exit(1)
