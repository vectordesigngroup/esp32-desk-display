"""Status bridge for the desk display.

Serves one JSON document. Bitcoin figures come from the local Bitcoin Core
plus mempool.space. Lightning channel balances are read from StartOS when
that machine answers SSH. Public Lightning totals come from mempool.space.
Pi stats and Frigate events are read over SSH. Recent Bitcoin posts come
from a public Nitter RSS search. Nothing here is written back to those machines.
"""

from __future__ import annotations

import html
import json
import re
import socket
import ssl
import subprocess
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from email.utils import parsedate_to_datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parent
CFG_PATH = ROOT / "bridge_config.json"
LOG_PATH = ROOT / "bridge.log"

BITCOIN_CLI = ""
BITCOIN_DIR = ""
PI_HOST = ""
PI_USER = ""
PI_KEY = ""
START_HOST = ""
START_USER = ""
START_KEY = ""

CACHE_SECONDS = 10
MARKET_TTL = 30
HASH_TTL = 60
POST_TTL = 180
HALVING_HEIGHT = 1_050_000
# Volunteer mirrors. Tried in order. Cert failures retry once without verification.
POST_HOSTS = (
    "https://nitter.mie.foundation/search/rss?f=tweets&q=",
    "https://nitter.mizuki.st/search/rss?f=tweets&q=",
)
HASH_SPANS = {"3d", "1w", "1m", "3m", "1y"}
PRICE_WINDOWS = {"1d": 86400, "1w": 7 * 86400, "1m": 30 * 86400, "3m": 90 * 86400, "1y": 365 * 86400}
PRICE_CURRENCIES = ("USD", "EUR", "GBP", "CAD", "CHF", "AUD", "JPY")
_cache = {"at": 0.0, "body": None, "key": ""}
_market = {"at": 0.0, "body": None, "span": ""}
_hash = {"at": 0.0, "eh": 0, "diff_t10": 0, "series": [], "span": ""}
_posts = {"ok": False, "items": [], "tag": "", "at": 0.0}
_want_tag = "bitcoin"
_spot: dict[str, int] = {}
_history = {"at": 0.0, "rows": []}
_lock = threading.Lock()


def clean_span(value: str) -> str:
    value = (value or "").strip().lower()
    return value if value in HASH_SPANS else "1m"


def clean_tag(value: str) -> str:
    raw = (value or "").strip().lstrip("#")
    cleaned = "".join(ch for ch in raw if ch.isalnum() or ch == "_")
    return (cleaned or "bitcoin")[:24]


def clean_pspan(value: str) -> str:
    value = (value or "").strip().lower()
    return value if value in PRICE_WINDOWS else "1d"


def clean_cur(value: str) -> str:
    value = (value or "").strip().upper()
    return value if value in PRICE_CURRENCIES else "USD"


def _downsample(values: list[int], count: int = 30) -> list[int]:
    if len(values) <= count or count < 2:
        return values
    last = len(values) - 1
    picked = []
    for i in range(count):
        picked.append(values[int(round(i * last / (count - 1)))])
    return picked


def log(msg: str) -> None:
    line = time.strftime("%Y-%m-%d %H:%M:%S ") + msg
    print(line, flush=True)
    try:
        if LOG_PATH.exists() and LOG_PATH.stat().st_size > 1_000_000:
            LOG_PATH.write_text("", encoding="utf-8")
        with LOG_PATH.open("a", encoding="utf-8") as fh:
            fh.write(line + "\n")
    except OSError:
        pass


def load_config() -> dict:
    return json.loads(CFG_PATH.read_text(encoding="utf-8"))


def apply_config(cfg: dict) -> None:
    global BITCOIN_CLI, BITCOIN_DIR, PI_HOST, PI_USER, PI_KEY, START_HOST, START_USER, START_KEY
    BITCOIN_CLI = str(cfg.get("bitcoin_cli") or "")
    BITCOIN_DIR = str(cfg.get("bitcoin_dir") or "")
    PI_HOST = str(cfg.get("pi_host") or "")
    PI_USER = str(cfg.get("pi_user") or "")
    PI_KEY = str(cfg.get("pi_key") or "")
    START_HOST = str(cfg.get("startos_host") or "")
    START_USER = str(cfg.get("startos_user") or "")
    START_KEY = str(cfg.get("startos_key") or "")


def port_open(host: str, port: int, timeout: float = 0.8) -> bool:
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(timeout)
    try:
        sock.connect((host, port))
        return True
    except OSError:
        return False
    finally:
        sock.close()


def ssh(host: str, user: str, key: str, remote: str, timeout: int = 12) -> subprocess.CompletedProcess:
    if not host or not user or not key:
        return subprocess.CompletedProcess(args=[], returncode=1, stdout=b"", stderr=b"")
    return subprocess.run(
        [
            "ssh",
            "-i",
            key,
            "-o",
            "IdentitiesOnly=yes",
            "-o",
            "BatchMode=yes",
            "-o",
            "ConnectTimeout=4",
            "-o",
            "StrictHostKeyChecking=accept-new",
            f"{user}@{host}",
            remote,
        ],
        capture_output=True,
        timeout=timeout,
    )


def bitcoin_cli(*args: str) -> dict:
    if not BITCOIN_CLI:
        raise OSError("bitcoin-cli is not configured")
    cmd = [BITCOIN_CLI]
    if BITCOIN_DIR:
        cmd.append(f"-datadir={BITCOIN_DIR}")
    cmd.extend(args)
    out = subprocess.check_output(
        cmd,
        timeout=12,
        stderr=subprocess.STDOUT,
    )
    return json.loads(out.decode("utf-8"))


def http_bytes(url: str, timeout: int = 8, limit: int = 150_000, unverified: bool = False) -> bytes:
    ctx = ssl._create_unverified_context() if unverified else ssl.create_default_context()
    req = urllib.request.Request(url, headers={"User-Agent": "desk-display/1.0"})
    with urllib.request.urlopen(req, timeout=timeout, context=ctx) as resp:
        raw = resp.read(limit + 1)
    if len(raw) > limit:
        raise ValueError("response too large")
    return raw


def _cert_failed(exc: BaseException) -> bool:
    seen: BaseException | None = exc
    for _ in range(4):
        if seen is None:
            return False
        if isinstance(seen, ssl.SSLCertVerificationError):
            return True
        nxt = getattr(seen, "reason", None)
        if not isinstance(nxt, BaseException):
            nxt = getattr(seen, "__cause__", None)
        seen = nxt if isinstance(nxt, BaseException) else None
    return False


def http_json(url: str, timeout: int = 8, limit: int = 150_000, unverified: bool = False):
    raw = http_bytes(url, timeout=timeout, limit=limit, unverified=unverified)
    return json.loads(raw.decode("utf-8"))


def _num(value, default: float = 0.0) -> float:
    try:
        return float(value)
    except (TypeError, ValueError):
        return default


def _tenths_pct(value) -> int:
    return int(round(_num(value) * 10))


def plain(text: str, limit: int) -> str:
    text = re.sub(r"(?is)<script.*?>.*?</script>", " ", text or "")
    text = re.sub(r"(?is)<style.*?>.*?</style>", " ", text)
    text = re.sub(r"(?s)<[^>]+>", " ", text)
    text = html.unescape(text).replace("\xa0", " ")
    cleaned = []
    for ch in text:
        if 32 <= ord(ch) <= 126:
            cleaned.append(ch)
        elif ch.isspace():
            cleaned.append(" ")
    text = re.sub(r" +", " ", "".join(cleaned)).strip()
    if len(text) > limit:
        text = text[: limit - 1].rstrip() + "..."
    return text


def _asof(value) -> str:
    if not value:
        return ""
    try:
        text = str(value).replace("Z", "+00:00")
        return datetime_from(text)
    except (TypeError, ValueError):
        return ""


def datetime_from(text: str) -> str:
    from datetime import datetime

    dt = datetime.fromisoformat(text)
    return dt.strftime("%b %d").replace(" 0", " ")


def _empty_market() -> dict:
    return {
        "ok": False,
        "price_ok": False,
        "chg_ok": False,
        "usd": 0,
        "chg10": 0,
        "count": 0,
        "vsize": 0,
        "fee_sat": 0,
        "next_tx": 0,
        "next_med100": 0,
        "hash_eh": 0,
        "diff_t10": 0,
        "series": [],
        "epoch_ok": False,
        "adj10": 0,
        "left": 0,
        "eta_min": 0,
        "prev10": 0,
        "halving": 0,
        "pool": "",
        "last_tx": 0,
        "last_age": 0,
        "last_reward": 0,
        "ln_ok": False,
        "ln_nodes": 0,
        "ln_ch": 0,
        "ln_btc": 0,
        "ln_ppm": 0,
        "ln_asof": "",
    }


def as_sat_msat(value) -> int:
    if value is None:
        return 0
    if isinstance(value, bool):
        return 0
    if isinstance(value, (int, float)):
        return int(value) // 1000
    digits = "".join(ch for ch in str(value) if ch.isdigit())
    return int(digits) // 1000 if digits else 0


def fee_sat_vb(feerate) -> int | None:
    if not isinstance(feerate, (int, float)) or feerate <= 0:
        return None
    # bitcoin-cli estimatesmartfee returns BTC/kvB. 1e8 sats/BTC / 1000 vB = 1e5.
    return max(1, int(round(float(feerate) * 100_000)))


def section_bitcoin() -> dict:
    body = {
        "online": False,
        "local": False,
        "blocks": 0,
        "headers": 0,
        "tip": 0,
        "progress": 0,
        "ibd": False,
        "pruned": False,
        "peers": 0,
        "mempool_tx": 0,
        "local_fee": 0,
        "fee_fast": 0,
        "fee_half": 0,
        "fee_hour": 0,
        "fee_econ": 0,
    }
    try:
        info = bitcoin_cli("getblockchaininfo")
        mem = bitcoin_cli("getmempoolinfo")
        net = bitcoin_cli("getnetworkinfo")
        body.update(
            {
                "online": True,
                "local": True,
                "blocks": info.get("blocks") or 0,
                "headers": info.get("headers") or 0,
                "progress": info.get("verificationprogress") or 0,
                "ibd": bool(info.get("initialblockdownload")),
                "pruned": bool(info.get("pruned")),
                "peers": net.get("connections") or 0,
                "mempool_tx": mem.get("size") or 0,
            }
        )
        try:
            est = bitcoin_cli("estimatesmartfee", "2", "economical")
            local_fee = fee_sat_vb(est.get("feerate"))
            if local_fee:
                body["local_fee"] = local_fee
        except (subprocess.SubprocessError, json.JSONDecodeError, OSError):
            pass
    except (subprocess.SubprocessError, json.JSONDecodeError, OSError) as exc:
        log(f"bitcoin local failed: {type(exc).__name__}")

    try:
        body["tip"] = int(http_json("https://mempool.space/api/blocks/tip/height"))
        if not body["online"]:
            body["online"] = True
            body["blocks"] = body["tip"]
            body["headers"] = body["tip"]
        fees = http_json("https://mempool.space/api/v1/fees/recommended")
        body["fee_fast"] = int(fees.get("fastestFee") or 0)
        body["fee_half"] = int(fees.get("halfHourFee") or 0)
        body["fee_hour"] = int(fees.get("hourFee") or 0)
        body["fee_econ"] = int(fees.get("economyFee") or 0)
    except (OSError, ValueError, TypeError) as exc:
        log(f"mempool.space failed: {type(exc).__name__}")
    return body


def _eta_minutes(value) -> int:
    # difficulty-adjustment remainingTime is milliseconds when it is this large.
    # A value already in seconds stays below that line for a normal epoch.
    raw = _num(value)
    if raw <= 0:
        return 0
    seconds = raw / 1000.0 if raw > 10_000_000 else raw
    return int(seconds / 60)


def section_market(span: str) -> dict:
    now = time.time()
    cached = _market.get("body")
    if cached and _market.get("span") == span and now - _market["at"] < MARKET_TTL:
        return cached
    body = dict(cached or _empty_market())

    try:
        prices = http_json("https://mempool.space/api/v1/prices", timeout=6, limit=4_000)
        usd = int(_num(prices.get("USD")))
        if usd > 0:
            body["usd"] = usd
            body["price_ok"] = True
        for cur in PRICE_CURRENCIES:
            value = int(round(_num(prices.get(cur))))
            if value > 0:
                _spot[cur] = value
    except (OSError, ValueError, TypeError) as exc:
        log(f"price failed: {type(exc).__name__}")

    try:
        ts = int(now) - 86400
        hist = http_json(
            f"https://mempool.space/api/v1/historical-price?currency=USD&timestamp={ts}",
            timeout=6,
            limit=8_000,
        )
        points = hist.get("prices") or []
        old = int(_num(points[0].get("USD"))) if points else 0
        usd = int(body.get("usd") or 0)
        if old > 0 and usd > 0:
            body["chg10"] = int(round((usd - old) * 1000.0 / old))
            body["chg_ok"] = True
    except (OSError, ValueError, TypeError) as exc:
        log(f"price history failed: {type(exc).__name__}")

    try:
        mem = http_json("https://mempool.space/api/mempool", timeout=6, limit=80_000)
        body["count"] = int(_num(mem.get("count")))
        body["vsize"] = int(_num(mem.get("vsize")))
        fee = mem.get("total_fee")
        # The live field is an integer satoshi total. A small float would be BTC.
        if isinstance(fee, float) and abs(fee) < 10_000:
            body["fee_sat"] = int(round(fee * 1e8))
        elif isinstance(fee, (int, float)):
            body["fee_sat"] = int(fee)
    except (OSError, ValueError, TypeError) as exc:
        log(f"mempool summary failed: {type(exc).__name__}")

    try:
        projected = http_json("https://mempool.space/api/v1/fees/mempool-blocks", timeout=6, limit=80_000)
        if isinstance(projected, list) and projected:
            nxt = projected[0]
            body["next_tx"] = int(_num(nxt.get("nTx")))
            body["next_med100"] = int(round(_num(nxt.get("medianFee")) * 100))
    except (OSError, ValueError, TypeError) as exc:
        log(f"next block failed: {type(exc).__name__}")

    try:
        diff = http_json("https://mempool.space/api/v1/difficulty-adjustment", timeout=6, limit=8_000)
        body["adj10"] = _tenths_pct(diff.get("difficultyChange"))
        body["prev10"] = _tenths_pct(diff.get("previousRetarget"))
        body["left"] = int(_num(diff.get("remainingBlocks")))
        body["eta_min"] = _eta_minutes(diff.get("remainingTime"))
        body["epoch_ok"] = True
    except (OSError, ValueError, TypeError) as exc:
        log(f"difficulty failed: {type(exc).__name__}")

    if _hash.get("span") != span or now - _hash["at"] >= HASH_TTL or not _hash["eh"]:
        try:
            mined = http_json(
                f"https://mempool.space/api/v1/mining/hashrate/{span}", timeout=8, limit=80_000
            )
            eh = int(round(_num(mined.get("currentHashrate")) / 1e18))
            diff_t10 = int(round(_num(mined.get("currentDifficulty")) / 1e11))
            if eh > 0:
                _hash["eh"] = eh
            if diff_t10 > 0:
                _hash["diff_t10"] = diff_t10
            series = []
            for point in mined.get("hashrates") or []:
                if not isinstance(point, dict):
                    continue
                value = int(round(_num(point.get("avgHashrate")) / 1e18))
                if value > 0:
                    series.append(value)
            _hash["series"] = _downsample(series, 30)
            _hash["span"] = span
            _hash["at"] = time.time()
        except (OSError, ValueError, TypeError) as exc:
            log(f"hashrate failed: {type(exc).__name__}")
            if _hash.get("span") != span:
                _hash["series"] = []
    body["hash_eh"] = _hash["eh"]
    body["diff_t10"] = _hash["diff_t10"]
    body["series"] = list(_hash["series"] if _hash.get("span") == span else [])

    try:
        blocks = http_json("https://mempool.space/api/v1/blocks", timeout=6, limit=150_000)
        if isinstance(blocks, list) and blocks:
            block = blocks[0]
            extras = block.get("extras") or {}
            pool = ((extras.get("pool") or {}).get("name")) or ""
            body["pool"] = plain(str(pool), 16)
            body["last_tx"] = int(_num(block.get("tx_count")))
            ts = _num(block.get("timestamp"))
            if ts > 1e12:
                ts /= 1000.0
            if ts > 0:
                body["last_age"] = max(0, int(time.time() - ts))
            body["last_reward"] = int(_num(extras.get("reward")))
            height = int(_num(block.get("height")))
            if height > 0:
                body["halving"] = max(0, HALVING_HEIGHT - height)
    except (OSError, ValueError, TypeError) as exc:
        log(f"blocks failed: {type(exc).__name__}")

    try:
        stats = http_json("https://mempool.space/api/v1/lightning/statistics/latest", timeout=6, limit=20_000)
        latest = stats.get("latest") if isinstance(stats, dict) else None
        if isinstance(latest, dict):
            cap = _num(latest.get("total_capacity"))
            body["ln_nodes"] = int(_num(latest.get("node_count")))
            body["ln_ch"] = int(_num(latest.get("channel_count")))
            body["ln_btc"] = int(round(cap / 1e8)) if cap else 0
            body["ln_ppm"] = int(_num(latest.get("avg_fee_rate")))
            body["ln_asof"] = _asof(latest.get("added"))
            body["ln_ok"] = body["ln_nodes"] > 0
    except (OSError, ValueError, TypeError) as exc:
        log(f"lightning stats failed: {type(exc).__name__}")

    body["ok"] = bool(
        body.get("price_ok") or body.get("epoch_ok") or body.get("hash_eh") or body.get("count") or body.get("ln_ok")
    )
    _market["body"] = body
    _market["span"] = span
    _market["at"] = time.time()
    return body


def _post_ago(pub: str) -> int:
    if not pub:
        return 0
    try:
        dt = parsedate_to_datetime(pub)
        return max(0, int(time.time() - dt.timestamp()))
    except (TypeError, ValueError, OverflowError):
        return 0


def fetch_posts(tag: str) -> list[dict]:
    import xml.etree.ElementTree as ET

    raw = b""
    last_exc: Exception | None = None
    query = urllib.parse.quote(tag)
    for url in (host + query for host in POST_HOSTS):
        try:
            try:
                raw = http_bytes(url, timeout=6, limit=80_000)
            except urllib.error.URLError as exc:
                if not _cert_failed(exc):
                    raise
                raw = http_bytes(url, timeout=6, limit=80_000, unverified=True)
            if raw:
                break
        except (OSError, ValueError) as exc:
            last_exc = exc
            raw = b""
    if not raw:
        if last_exc is not None:
            log(f"posts failed: {type(last_exc).__name__}")
        return []
    try:
        root = ET.fromstring(raw)
    except ET.ParseError:
        log("posts failed: ParseError")
        return []
    gathered = []
    seen = set()
    for item in root.findall(".//item"):
        creator = plain(item.findtext("{http://purl.org/dc/elements/1.1/}creator") or "", 18)
        creator = creator.lstrip("@")
        desc = plain(item.findtext("description") or "", 140)
        title = plain(item.findtext("title") or "", 140)
        text = desc or title
        if creator and text.lower().startswith(creator.lower() + ":"):
            text = text[len(creator) + 1 :].strip()
        if not text:
            continue
        key = text[:40].lower()
        if key in seen:
            continue
        seen.add(key)
        gathered.append({"user": creator or "post", "text": text, "ago": _post_ago(item.findtext("pubDate") or "")})
        if len(gathered) >= 12:
            break
    gathered.sort(key=lambda it: it["ago"])
    fresh = [it for it in gathered if it["ago"] <= 36 * 3600]
    return (fresh or gathered)[:3]


def refresh_posts(tag: str) -> None:
    try:
        items = fetch_posts(tag)
    except Exception as exc:
        log(f"posts failed: {type(exc).__name__}")
        items = []
    with _lock:
        if items:
            _posts["items"] = items
            _posts["ok"] = True
            _posts["tag"] = tag
        elif _posts.get("tag") != tag:
            _posts["items"] = []
            _posts["ok"] = False
            _posts["tag"] = tag
        _posts["at"] = time.time()
    log(f"posts {len(items)} {tag}")


def posts_loop() -> None:
    while True:
        with _lock:
            tag = _want_tag
            due = _posts.get("tag") != tag or time.time() - float(_posts.get("at") or 0) >= POST_TTL
        if due:
            refresh_posts(tag)
        time.sleep(5)


def refresh_price_history() -> None:
    try:
        payload = http_json("https://mempool.space/api/v1/historical-price", timeout=25, limit=5_000_000)
    except (OSError, ValueError, TypeError) as exc:
        log(f"price history failed: {type(exc).__name__}")
        return
    rows = []
    for point in payload.get("prices") or []:
        if not isinstance(point, dict):
            continue
        ts = int(_num(point.get("time")))
        if ts <= 0:
            continue
        vals = []
        useful = False
        for cur in PRICE_CURRENCIES:
            raw = _num(point.get(cur))
            value = int(round(raw)) if raw > 0 else 0
            if value > 0:
                useful = True
            vals.append(value)
        if useful:
            rows.append((ts, *vals))
    rows.sort(key=lambda row: row[0])
    with _lock:
        _history["rows"] = rows
        _history["at"] = time.time()
    log(f"prices {len(rows)}")


def price_loop() -> None:
    while True:
        with _lock:
            due = not _history.get("rows") or time.time() - float(_history.get("at") or 0) >= 600
        if due:
            refresh_price_history()
        time.sleep(20)


def section_quote(cur: str, pspan: str) -> dict:
    # Caller holds _lock. History is refreshed on another thread.
    window = PRICE_WINDOWS[pspan]
    index = PRICE_CURRENCIES.index(cur) + 1
    cutoff = time.time() - window
    values = [row[index] for row in _history.get("rows") or [] if row[0] >= cutoff and row[index] > 0]
    series = _downsample(values, 30)
    spot = int(_spot.get(cur) or 0)
    if spot > 0:
        if series:
            series[-1] = spot
        else:
            series = [spot]
    first = values[0] if values else 0
    last = spot or (values[-1] if values else 0)
    out = {
        "qcur": cur,
        "qspan": pspan,
        "quote": last,
        "quote_ok": last > 0,
        "quotes": series,
        "qchg_ok": False,
        "qchg10": 0,
    }
    if first > 0 and last > 0 and len(values) >= 2:
        out["qchg10"] = int(round((last - first) * 1000.0 / first))
        out["qchg_ok"] = True
    return out


def pi_stats() -> dict:
    body = {
        "up": False,
        "load": 0,
        "mem": 0,
        "disk": 0,
        "uptime": 0,
        "stats": False,
        "jellyfin": False,
        "vaultwarden": False,
        "frigate": False,
    }
    if not PI_HOST:
        return body
    body["jellyfin"] = port_open(PI_HOST, 8096)
    body["vaultwarden"] = port_open(PI_HOST, 8222)
    body["frigate"] = port_open(PI_HOST, 8971)
    body["up"] = body["jellyfin"] or body["vaultwarden"] or body["frigate"]
    if not body["up"] and not port_open(PI_HOST, 22):
        return body
    remote = (
        "awk '{print \"LOAD \" $1; exit}' /proc/loadavg; "
        "awk '/MemTotal/{t=$2} /MemAvailable/{a=$2} END{printf \"MEM %d\\n\", (t-a)*100/t}' /proc/meminfo; "
        "df -P / | awk 'NR==2{gsub(/%/,\"\",$5); printf \"DISK %s\\n\", $5}'; "
        "awk '{printf \"UP %d\\n\", $1}' /proc/uptime"
    )
    try:
        done = ssh(PI_HOST, PI_USER, PI_KEY, remote, timeout=8)
        if done.returncode != 0:
            log("pi ssh failed")
            return body
        for line in done.stdout.decode("utf-8", "replace").splitlines():
            parts = line.split()
            if len(parts) != 2:
                continue
            key, val = parts
            if key == "LOAD":
                body["load"] = float(val)
                body["stats"] = True
            elif key == "MEM":
                body["mem"] = int(float(val))
            elif key == "DISK":
                body["disk"] = int(float(val))
            elif key == "UP":
                body["uptime"] = int(float(val))
        body["up"] = True
    except (subprocess.SubprocessError, OSError, ValueError) as exc:
        log(f"pi stats failed: {type(exc).__name__}")
    return body


def _b64_python(source: str) -> str:
    import base64

    encoded = base64.b64encode(source.encode("utf-8")).decode("ascii")
    return f"echo {encoded} | base64 -d | docker exec -i frigate python3"


def frigate_event() -> dict:
    empty = {
        "ok": False,
        "id": "",
        "camera": "",
        "label": "",
        "score": None,
        "start": 0,
        "end": 0,
        "ended": False,
        "ago": 0,
        "has_snapshot": False,
    }
    if not PI_HOST or not port_open(PI_HOST, 22):
        return empty
    source = r"""
import json, urllib.request, time
raw = urllib.request.urlopen("http://127.0.0.1:5000/api/events?limit=1&include_thumbnails=0", timeout=6).read()
ev = json.loads(raw)
if not ev:
    print(json.dumps({"ok": False}))
else:
    e = ev[0]
    data = e.get("data") or {}
    score = e.get("top_score")
    if score is None:
        score = data.get("top_score")
    if score is None:
        score = data.get("score")
    print(json.dumps({
        "ok": True,
        "id": e.get("id") or "",
        "camera": e.get("camera") or "",
        "label": e.get("label") or "",
        "score": score,
        "start": e.get("start_time") or 0,
        "end": e.get("end_time") or 0,
        "has_snapshot": bool(e.get("has_snapshot")),
    }))
"""
    try:
        done = ssh(PI_HOST, PI_USER, PI_KEY, _b64_python(source), timeout=15)
        if done.returncode != 0 or not done.stdout.strip():
            log("frigate query failed")
            return empty
        event = json.loads(done.stdout.decode("utf-8", "replace").strip().splitlines()[-1])
        if not event.get("ok"):
            return empty
        now = time.time()
        start = float(event.get("start") or 0)
        end = float(event.get("end") or 0)
        event["ended"] = end > 0
        event["ago"] = max(0, int(now - start)) if start else 0
        return event
    except (subprocess.SubprocessError, OSError, json.JSONDecodeError, ValueError) as exc:
        log(f"frigate failed: {type(exc).__name__}")
        return empty


def safe_name(name: str) -> str | None:
    if name and all(ch.isalnum() or ch in ".-_" for ch in name):
        return name
    return None


def container_names() -> list[str]:
    for tool in ("podman", "docker"):
        done = ssh(START_HOST, START_USER, START_KEY, f"sudo {tool} ps --format '{{{{.Names}}}}'", timeout=10)
        if done.returncode == 0 and done.stdout.strip():
            return [line.strip() for line in done.stdout.decode().splitlines() if line.strip()]
    return []


def lightning() -> dict:
    body = {"startos": False, "cln": None, "lnd": None}
    if not START_HOST or not port_open(START_HOST, 22, 0.8):
        return body
    body["startos"] = True
    try:
        names = container_names()
    except (subprocess.SubprocessError, OSError) as exc:
        log(f"startos containers failed: {type(exc).__name__}")
        return body
    cln = next((n for n in names if "lnd" not in n.lower() and any(s in n.lower() for s in ("c-lightning", "core-lightning", "corelightning", "cln"))), None)
    lnd = next((n for n in names if "lnd" in n.lower()), None)
    if cln and safe_name(cln):
        body["cln"] = read_cln(cln)
    if lnd and safe_name(lnd):
        body["lnd"] = read_lnd(lnd)
    return body


def read_cln(container: str) -> dict | None:
    done = ssh(
        START_HOST,
        START_USER,
        START_KEY,
        f"sudo podman exec {container} lightning-cli listfunds",
        timeout=12,
    )
    if done.returncode != 0:
        done = ssh(
            START_HOST,
            START_USER,
            START_KEY,
            f"sudo docker exec {container} lightning-cli listfunds",
            timeout=12,
        )
    if done.returncode != 0:
        return None
    try:
        data = json.loads(done.stdout.decode("utf-8", "replace"))
    except json.JSONDecodeError:
        return None
    channels = data.get("channels") or []
    local = sum(as_sat_msat(ch.get("our_amount_msat")) for ch in channels if isinstance(ch, dict))
    return {"channels": len(channels), "local_sat": local}


def read_lnd(container: str) -> dict | None:
    remote = (
        f"sudo podman exec {container} lncli channelbalance; echo '---SPLIT---'; "
        f"sudo podman exec {container} lncli getinfo"
    )
    done = ssh(START_HOST, START_USER, START_KEY, remote, timeout=15)
    if done.returncode != 0 or b"---SPLIT---" not in done.stdout:
        remote = (
            f"sudo docker exec {container} lncli channelbalance; echo '---SPLIT---'; "
            f"sudo docker exec {container} lncli getinfo"
        )
        done = ssh(START_HOST, START_USER, START_KEY, remote, timeout=15)
    if b"---SPLIT---" not in done.stdout:
        return None
    left, right = done.stdout.split(b"---SPLIT---", 1)
    try:
        bal = json.loads(left.decode("utf-8", "replace"))
        info = json.loads(right.decode("utf-8", "replace"))
    except json.JSONDecodeError:
        return None
    sat = bal.get("local_balance", {}).get("sat", bal.get("balance", 0))
    try:
        sat = int(sat)
    except (TypeError, ValueError):
        sat = 0
    channels = int(info.get("num_active_channels") or 0)
    return {"channels": channels, "local_sat": sat}


def build_status(span: str = "1m", tag: str = "bitcoin", pspan: str = "1d", cur: str = "USD") -> dict:
    with _lock:
        return _build_status(span, tag, pspan, cur)


def _build_status(span: str, tag: str, pspan: str, cur: str) -> dict:
    global _want_tag
    now = time.time()
    key = span + "|" + tag + "|" + pspan + "|" + cur
    _want_tag = tag
    if _cache["body"] is not None and _cache.get("key") == key and now - _cache["at"] < CACHE_SECONDS:
        return _cache["body"]

    btc = section_bitcoin()
    market = dict(section_market(span))
    market.update(section_quote(cur, pspan))
    tip = int(btc.get("tip") or btc.get("blocks") or 0)
    if tip > 0:
        market["halving"] = max(0, HALVING_HEIGHT - tip)
    pi = pi_stats()
    light = lightning()
    startos_up = bool(light["startos"]) or (bool(START_HOST) and port_open(START_HOST, 443, 0.8))
    if _posts.get("tag") == tag:
        posts = {"ok": bool(_posts.get("ok")), "items": list(_posts.get("items") or [])}
    else:
        posts = {"ok": False, "items": []}
    body = {
        "ok": True,
        "now": int(now),
        "bitcoin": btc,
        "market": market,
        "posts": posts,
        "lightning": {
            "startos": startos_up,
            "cln": light["cln"],
            "lnd": light["lnd"],
        },
        "servers": {
            "pi": {
                "up": pi["up"],
                "load": pi["load"],
                "mem": pi["mem"],
                "disk": pi["disk"],
                "uptime": pi["uptime"],
                "stats": pi["stats"],
            },
            "jellyfin": pi["jellyfin"],
            "vaultwarden": pi["vaultwarden"],
            "frigate": pi["frigate"],
            "startos": startos_up,
        },
    }
    _cache["at"] = time.time()
    _cache["key"] = key
    _cache["body"] = body
    log(
        "status blocks={blocks} price={price} chg10={chg} hash={hash} points={points} span={span} posts={posts} tag={tag} quote={quote} {cur} {pspan} qpoints={qpoints} pi={pi} disk={disk} startos={startos}".format(
            blocks=btc.get("blocks"),
            price=market.get("usd"),
            chg=market.get("chg10"),
            hash=market.get("hash_eh"),
            points=len(market.get("series") or []),
            span=span,
            posts=len(posts["items"]),
            tag=tag,
            quote=market.get("quote"),
            cur=cur,
            pspan=pspan,
            qpoints=len(market.get("quotes") or []),
            pi="up" if pi["up"] else "down",
            disk=pi.get("disk"),
            startos="up" if startos_up else "down",
        )
    )
    return body


class Handler(BaseHTTPRequestHandler):
    token = ""

    def log_message(self, fmt: str, *args) -> None:
        return

    def _authorized(self) -> bool:
        import hmac

        got = self.headers.get("X-Desk-Key", "")
        return hmac.compare_digest(got, self.token)

    def do_GET(self) -> None:
        path = self.path.split("?", 1)[0]
        if path == "/health":
            self._send(200, b'{"ok":true}', "application/json")
            return
        if not self._authorized():
            self._send(401, b'{"ok":false}', "application/json")
            return
        if path == "/status":
            query = urllib.parse.parse_qs(urllib.parse.urlsplit(self.path).query)
            span = clean_span((query.get("span") or ["1m"])[0])
            tag = clean_tag((query.get("tag") or ["bitcoin"])[0])
            pspan = clean_pspan((query.get("pspan") or ["1d"])[0])
            cur = clean_cur((query.get("cur") or ["USD"])[0])
            body = build_status(span, tag, pspan, cur)
            self._send(200, json.dumps(body).encode("utf-8"), "application/json")
            return
        self._send(404, b'{"ok":false}', "application/json")

    def _send(self, code: int, payload: bytes, content_type: str) -> None:
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        if payload:
            self.wfile.write(payload)


def main() -> int:
    cfg = load_config()
    apply_config(cfg)
    token = str(cfg.get("token") or "")
    port = int(cfg.get("port") or 8765)
    if len(token) < 16:
        log("missing token")
        return 1
    Handler.token = token
    try:
        server = ThreadingHTTPServer(("0.0.0.0", port), Handler)
    except OSError as exc:
        log(f"bind failed: {exc.errno}")
        return 0 if getattr(exc, "winerror", None) == 10048 or exc.errno in (98, 10048) else 1
    threading.Thread(target=posts_loop, name="posts", daemon=True).start()
    threading.Thread(target=price_loop, name="prices", daemon=True).start()
    log(f"listening on {port}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        return 0
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
