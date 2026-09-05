#!/usr/bin/env python3
"""Yoyu companion — runs on the computer where you use Claude Code.

Reads the *real* Claude subscription usage numbers and pushes them to the Pi.

How (and why it works when a fresh sign-in doesn't): it never signs in. It
reuses Claude Code's own existing login — the credentials Claude Code already
saved on this machine — refreshes that token if needed, and reads Anthropic's
usage endpoint. It never touches the authorization-code sign-in exchange, which
is the throttled one. (Same approach as the Sparko "Fuel" widget.)

    Credentials:  macOS Keychain item "Claude Code-credentials", else
                  ~/.claude/.credentials.json  (Windows/Linux)
    Refresh:      POST https://platform.claude.com/v1/oauth/token (refresh_token)
    Usage:        GET  https://api.anthropic.com/api/oauth/usage

If it can't read credentials (Claude Code not logged in here), it falls back to
estimating usage from Claude Code's local logs.

Run it:  python3 companion.py --pi http://yoyu.local:8080
Standard library only, Python 3.8+.
"""

import argparse
import concurrent.futures
import datetime
import glob
import hashlib
import hmac
import json
import os
import secrets
import shutil
import socket
import ssl
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

# TLS trust store. A PyInstaller-frozen binary (esp. on macOS) often can't find
# the system CA certs -> "CERTIFICATE_VERIFY_FAILED". Prefer certifi's bundle
# when present (it is in the packaged app); fall back to the system default for
# a plain "run from source" where certifi may not be installed.
try:
    import certifi
    _SSL_CONTEXT = ssl.create_default_context(cafile=certifi.where())
except Exception:  # noqa: BLE001
    _SSL_CONTEXT = ssl.create_default_context()

APP_MARKER = "Yoyu"  # /api/status "app" field, used for discovery
# Boards flashed before the rename still answer with these. Discovery has to
# keep accepting them or a working board on the desk becomes undiscoverable
# after a companion update -- and the fix would be a USB re-flash.
LEGACY_MARKERS = ("Headroom", "ClaudeTrackerPi")

CLIENT_ID = "9d1c250a-e61b-44d9-88ed-5944d1962f5e"
REFRESH_URL = "https://platform.claude.com/v1/oauth/token"
USAGE_URL = "https://api.anthropic.com/api/oauth/usage"
OAUTH_BETA = "oauth-2025-04-20"
USER_AGENT = "Yoyu-Companion/1.0"
KEYCHAIN_SERVICE = "Claude Code-credentials"
REFRESH_MARGIN = 300  # refresh if the token expires within 5 minutes

WINDOW_LABELS = {
    "five_hour": "Session (5 hour)",
    "seven_day": "Weekly (all models)",
    "seven_day_sonnet": "Weekly (Sonnet)",
    "seven_day_opus": "Weekly (Opus)",
    "seven_day_fable": "Weekly (Fable)",
    "seven_day_oauth_apps": "Weekly (connected apps)",
}

# Fallback-only: rough token budgets if we must estimate from logs. Anthropic
# doesn't publish real caps, so these are ballpark; "max" is ~5x "pro". Only
# used when there's no Claude Code login to read the real numbers from.
PLAN_PRESETS = {
    "max": {
        "five_hour": 220_000_000,
        "seven_day": 1_500_000_000,
        "seven_day_opus": 300_000_000,
    },
    "pro": {
        "five_hour": 44_000_000,
        "seven_day": 300_000_000,
        "seven_day_opus": 60_000_000,
    },
}
DEFAULT_LIMITS = PLAN_PRESETS["max"]
FIVE_HOURS, SEVEN_DAYS = 5 * 3600, 7 * 86400


# ----------------------------------------------------- credentials (like Sparko)

def _creds_file():
    return os.path.join(os.path.expanduser("~"), ".claude", ".credentials.json")


def read_creds():
    """Return (creds, save_fn) or (None, None). creds has accessToken/
    refreshToken/expiresAt(ms). save_fn persists an updated oauth dict."""
    # macOS Keychain
    if sys.platform == "darwin":
        try:
            out = subprocess.run(
                ["security", "find-generic-password", "-s", KEYCHAIN_SERVICE, "-w"],
                capture_output=True, text=True, timeout=5)
            if out.returncode == 0 and out.stdout.strip():
                creds = _parse_creds(out.stdout)
                if creds:
                    def save(oauth):
                        blob = json.dumps({"claudeAiOauth": oauth})
                        subprocess.run(
                            ["security", "add-generic-password", "-U",
                             "-s", KEYCHAIN_SERVICE, "-a", KEYCHAIN_SERVICE,
                             "-w", blob], capture_output=True, timeout=5)
                    return creds, save
        except (OSError, subprocess.SubprocessError):
            pass
    # file (Windows / Linux)
    path = _creds_file()
    try:
        with open(path, "r", encoding="utf-8") as fh:
            creds = _parse_creds(fh.read())
        if creds:
            def save(oauth):
                data = {"claudeAiOauth": oauth}
                tmp = path + ".tmp"
                # Create the temp file 0600 up front so the token is never
                # briefly world-readable, then preserve that on the final file
                # (os.replace would otherwise leave it at the umask default,
                # downgrading Claude's original 0600 credentials file).
                fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
                with os.fdopen(fd, "w", encoding="utf-8") as fh:
                    json.dump(data, fh)
                os.replace(tmp, path)
                try:
                    os.chmod(path, 0o600)
                except OSError:
                    pass  # best effort (e.g. Windows)
            return creds, save
    except OSError:
        pass
    return None, None


def _parse_creds(raw):
    try:
        j = json.loads(raw)
    except ValueError:
        return None
    o = j.get("claudeAiOauth") if isinstance(j, dict) else None
    o = o or j
    access = o.get("accessToken") or o.get("access_token")
    if not access:
        return None
    expires = o.get("expiresAt") or o.get("expires_at") or 0
    return {
        "accessToken": access,
        "refreshToken": o.get("refreshToken") or o.get("refresh_token"),
        "expiresAt": int(expires),  # epoch ms
        "subscriptionType": o.get("subscriptionType"),
        "_raw": o,
    }


def valid_token(creds, save_fn, force=False):
    """Return a usable access token, refreshing if expired. None on fail.

    `force` refreshes regardless of the clock. Needed because expiresAt is not
    the only way a token dies: refresh tokens rotate, so anything else using
    this login can invalidate ours while our own timestamp still says it is
    good. In that state the check below happily hands back a dead token
    forever.
    """
    exp_s = creds["expiresAt"] / 1000.0 if creds["expiresAt"] else 0
    if not force and exp_s and exp_s - REFRESH_MARGIN > time.time():
        return creds["accessToken"]            # still fresh — pure read
    if not creds["refreshToken"]:
        return creds["accessToken"] if not exp_s else None
    # Rotating refresh tokens: only refresh if we can write the new one back,
    # so we never leave Claude Code with a dead token.
    try:
        body = json.dumps({"grant_type": "refresh_token",
                           "refresh_token": creds["refreshToken"],
                           "client_id": CLIENT_ID}).encode("utf-8")
        req = urllib.request.Request(
            REFRESH_URL, data=body,
            headers={"Content-Type": "application/json",
                     "User-Agent": USER_AGENT}, method="POST")
        with urllib.request.urlopen(req, timeout=20, context=_SSL_CONTEXT) as resp:
            result = json.loads(resp.read().decode("utf-8"))
    except (urllib.error.URLError, OSError, ValueError) as exc:
        # A 400 here almost always means the refresh token was already spent by
        # something else -- most often a Yoyu board signed in to the same Claude
        # account, which rotates this computer's token out from under it. Worth
        # naming, because the obvious reading ("network problem") sends people
        # looking in the wrong place entirely.
        print(f"token refresh failed ({exc})", file=sys.stderr)
        if "400" in str(exc):
            print("  This usually means something else already used this "
                  "login's refresh token -", file=sys.stderr)
            print("  commonly a Yoyu board signed in to the same Claude "
                  "account.", file=sys.stderr)
        print("  Sign in again: run  claude  in a terminal, then type  /login .",
              file=sys.stderr)
        return None
    oauth = dict(creds["_raw"])
    oauth["accessToken"] = result["access_token"]
    if result.get("refresh_token"):
        oauth["refreshToken"] = result["refresh_token"]
    if result.get("expires_in"):
        oauth["expiresAt"] = int((time.time() + result["expires_in"]) * 1000)
    try:
        save_fn(oauth)
    except Exception as exc:  # noqa: BLE001 - don't lose the token on write fail
        print(f"warning: refreshed but couldn't save back ({exc})",
              file=sys.stderr)
    return oauth["accessToken"]


def fetch_usage(token):
    req = urllib.request.Request(
        USAGE_URL,
        headers={"Authorization": f"Bearer {token}",
                 "anthropic-beta": OAUTH_BETA,
                 "Accept": "application/json",
                 "User-Agent": USER_AGENT})
    with urllib.request.urlopen(req, timeout=20, context=_SSL_CONTEXT) as resp:
        return json.loads(resp.read().decode("utf-8"))


def _window_label(key):
    """Friendly name for a usage window. Known windows are named explicitly;
    any future per-model weekly window (seven_day_<model>, e.g. seven_day_fable)
    is turned into "Weekly (<Model>)" rather than a raw "Seven Day Fable"."""
    if key in WINDOW_LABELS:
        return WINDOW_LABELS[key]
    if key.startswith("seven_day_"):
        model = key[len("seven_day_"):].replace("_", " ").title()
        return f"Weekly ({model})"
    return key.replace("_", " ").title()


# Not usage windows, whatever they look like. "spend" is an object of a
# different shape entirely, and "extra_usage" is money -- see
# credits_from_usage() for why it must never reach the window list.
NOT_WINDOWS = ("extra_usage", "spend", "limits", "member_dashboard_available")


def windows_from_usage(raw):
    order = list(WINDOW_LABELS)
    out = []
    for key, value in (raw or {}).items():
        if key in NOT_WINDOWS:
            continue
        if not isinstance(value, dict):
            continue
        util = value.get("utilization")
        if util is None:
            continue
        # Skip entries that are both unrecognised and information-free.
        #
        # The endpoint returns internal codenames alongside the real windows --
        # nimbus_quill, tangelo, amber_ladder, cinder_cove and friends -- and
        # _window_label happily Title-Cases them, so a board with three meter
        # slots was spending one on "Nimbus Quill 0%". That says nothing to
        # anyone and crowds out a window that does.
        #
        # Deliberately conservative, because guessing wrong hides a real limit.
        # A window Anthropic actually ships has a reset time; that is what
        # makes it a window. So a KNOWN key always shows, an unknown key WITH
        # a reset time shows (the seven_day_<model> case this code already
        # goes out of its way to handle), and only an unknown key with no
        # reset and nothing used is dropped.
        if (key not in WINDOW_LABELS
                and not (value.get("resets_at") or value.get("resetsAt"))
                and not util):
            continue
        try:
            util = max(0.0, min(100.0, float(util)))
        except (TypeError, ValueError):
            continue
        out.append({
            "key": key,
            "label": _window_label(key),
            "utilization": round(util, 1),
            "resets_at": value.get("resets_at") or value.get("resetsAt"),
        })
    out.sort(key=lambda w: order.index(w["key"]) if w["key"] in order else 99)
    return out


def credits_from_usage(raw):
    """What happens after the plan limits run out: money.

    This is not a usage window and must never be treated as one. Every window
    answers "how much room is left before you are stopped"; this answers "how
    much have you now spent", which is the opposite side of the same moment --
    you only reach it because a window ran out. Feeding it through the window
    list would put it in front of the mascot, whose whole job is headroom, and
    the board would show a cheerful character while the meter ran.

    Anthropic reports it twice. `extra_usage` carries `utilization: null`, and
    windows_from_usage drops anything with a null utilization -- which is why
    "Extra usage" has sat in WINDOW_LABELS all this time as a label nothing
    could ever render. `spend` is the richer of the two and is already in minor
    units, so it is the one read here.

    Returns None when credits are switched off or the account has none, which
    is the common case and should show nothing at all.
    """
    sp = (raw or {}).get("spend")
    if not isinstance(sp, dict):
        return None
    used = sp.get("used") or {}
    limit = sp.get("limit") or {}
    if used.get("amount_minor") is None:
        return None
    try:
        used_minor = int(used["amount_minor"])
        limit_minor = (int(limit["amount_minor"])
                       if limit.get("amount_minor") is not None else None)
    except (TypeError, ValueError):
        return None
    # "enabled: false" means credits can no longer be SPENT -- the cap was hit,
    # or an org switched them off. It does not mean the account never had any,
    # and treating the two the same hid $41.24 of real spend at the exact
    # moment the figure mattered most. Money already gone is still money gone.
    #
    # So: nothing to show only when there is genuinely nothing -- not available
    # AND nothing spent.
    if not sp.get("enabled") and not used_minor:
        return None
    pct = sp.get("percent")
    if pct is None and limit_minor:
        pct = 100.0 * used_minor / limit_minor
    out = {
        "used_minor": used_minor,
        "limit_minor": limit_minor,
        # Minor units mean nothing without knowing where the point goes, and
        # it is per-currency: 100 minor units is $1.00 but ¥100.
        "exponent": int(used.get("exponent") or 2),
        "currency": used.get("currency") or "USD",
        "percent": round(float(pct), 1) if pct is not None else None,
        "limit_reached": bool(sp.get("severity") == "critical"),
        # Whether more can still be spent. False with a non-zero used_minor is
        # the stopped state: you spent this, and it has now cut you off.
        "available": bool(sp.get("enabled")),
    }
    return out


class LiveUnavailable(Exception):
    """A Claude Code login exists but live usage is temporarily unreadable.
    We must NOT fall back to log estimates in this case — stale real numbers
    on the tracker beat fresh wrong ones."""

    def __init__(self, msg, retry_after=0, rate_limited=False):
        super().__init__(msg)
        self.retry_after = retry_after
        self.rate_limited = rate_limited


def get_live_windows():
    """Real usage via Claude Code's login.

    Returns (windows, plan, credits), or None when there's no login at all.
    Raises LiveUnavailable on transient failure. `credits` is None unless the
    account actually has usage credits switched on.
    """
    creds, save_fn = read_creds()
    if not creds:
        return None
    # Two attempts at most. The second only happens on a 401, and only after
    # forcing a refresh: a rejected token whose expiresAt still looks fine
    # means it was rotated out from under us, not that it aged out, and the
    # clock-based check will keep returning the same dead token until somebody
    # notices the boards have gone quiet and runs `claude /login` by hand.
    # One refresh fixes it without anyone being told anything.
    raw = None
    for attempt in (0, 1):
        if attempt:
            # Re-read first: the earlier call may already have written a new
            # refresh token back, and refreshing with the stale one in memory
            # would spend a token that is no longer current.
            creds, save_fn = read_creds()
            if not creds:
                raise LiveUnavailable("Claude Code's login went away mid-read")
        token = valid_token(creds, save_fn, force=bool(attempt))
        if not token:
            raise LiveUnavailable(
                "Claude Code's login here needs signing in again — run "
                "`claude`, then /login")
        try:
            raw = fetch_usage(token)
            break
        except urllib.error.HTTPError as exc:
            if exc.code == 401 and attempt == 0:
                print("access token was rejected; refreshing and retrying once",
                      file=sys.stderr)
                continue
            retry_after = 0
            try:
                retry_after = int(exc.headers.get("Retry-After", 0) or 0)
            except (TypeError, ValueError):
                pass
            detail = ""
            try:
                detail = exc.read().decode("utf-8", "replace")[:200]
            except Exception:  # noqa: BLE001
                pass
            extra = ""
            if exc.code == 401:
                # Second 401 after a real refresh: the refresh token is spent
                # too, and nothing here can mint another one.
                extra = (" — a refresh did not help, so this login has been "
                         "signed out elsewhere. Run `claude`, then /login")
            raise LiveUnavailable(
                f"usage endpoint returned HTTP {exc.code}"
                + (f", retry after {retry_after}s" if retry_after else "")
                + (f" — {detail}" if detail else "") + extra,
                retry_after=retry_after, rate_limited=(exc.code == 429))
        except (urllib.error.URLError, OSError, ValueError) as exc:
            raise LiveUnavailable(f"couldn't read usage: {exc}")
    windows = windows_from_usage(raw)
    if not windows:
        raise LiveUnavailable("usage response had no windows")
    return windows, creds.get("subscriptionType"), credits_from_usage(raw)


# ------------------------------------------------- fallback: estimate from logs

def _parse_ts(value):
    try:
        return datetime.datetime.fromisoformat(
            str(value).replace("Z", "+00:00")).timestamp()
    except (ValueError, TypeError):
        return None


def _project_key(entry, path, root):
    """The full directory identifying the project an event belongs to.

    Prefer the event's own `cwd` — the folder under ~/.claude/projects is
    path-mangled (H:\\Projects\\Kiosk Grand becomes H--Projects-Kiosk-Grand)
    and can't be reversed: there is no telling a '-' that was a separator from
    one that was in the folder name. `cwd` is on every event and is exact, so
    the mangled slug is only a fallback for entries that somehow lack it.

    This returns the whole path, not the basename: ~/work/client-a/web and
    ~/work/client-b/web are different projects that a basename would merge."""
    cwd = entry.get("cwd")
    if isinstance(cwd, str) and cwd.strip():
        return cwd.strip().rstrip("/\\").replace("\\", "/")
    rel = os.path.relpath(path, root)
    return rel.replace("\\", "/").split("/")[0]


def _project_name(key):
    """The last path segment — what a project is called when nothing collides."""
    base = key.rstrip("/").split("/")[-1]
    if not base:
        return key
    # A bare slug fallback ('H--Projects-Sparko') has no separators to split on;
    # take the tail and accept that it may be approximate.
    return base.strip("-").split("-")[-1] if "/" not in key and "-" in base else base


def _roll_up_nested(totals):
    """Fold each project into the nearest ancestor that is also a project.

    Claude Code keys a project directory off the cwd, so opening a repo, a
    subdirectory of it, and a package inside that yields three "projects" that
    are one thing to the person who owns them — split across three rows and
    understated in each.

    Only folds into an ancestor that is *itself* tracked. H:/Projects/Qibb/Audio
    to Video stays put when there is no H:/Projects/Qibb project, because
    inventing a grouping the user never worked in would be a different kind of
    wrong. Matching is case-insensitive: Windows hands back whatever casing the
    shell used, and H:/Projects and h:/projects are the same directory."""
    canon = {}                                  # lowercased path -> real key
    for k in totals:
        canon.setdefault(k.lower(), k)
    root_of = {}
    for k in sorted(totals, key=len):           # ancestors are shorter, so
        cur, found = k, None                    # they resolve first
        while "/" in cur:
            cur = cur.rsplit("/", 1)[0]
            owner = canon.get(cur.lower())
            if owner is not None and owner != k:
                found = root_of.get(owner, owner)
                break
        root_of[k] = found or k
    merged = {}
    for k, tok in totals.items():
        r = root_of[k]
        merged[r] = merged.get(r, 0) + tok
    return merged


def _label_projects(keys, width=21):
    """Board labels for a set of project paths: short, and never ambiguous.

    Two projects that share a basename get qualified by their parent, and
    anything still colliding after the board's width limit gets a numeric
    suffix — two identical rows meaning different things is worse than an
    ugly one."""
    names = {k: _project_name(k) for k in keys}
    seen = {}
    for k, n in names.items():
        seen.setdefault(n, []).append(k)
    for n, owners in seen.items():
        if len(owners) < 2:
            continue
        for k in owners:                       # qualify with the parent dir
            parts = k.rstrip("/").split("/")
            if len(parts) >= 2:
                names[k] = parts[-2] + "/" + n

    out, used = {}, {}
    for k, n in names.items():
        if len(n) > width:
            # Trim from the *end* of each part, never the front. A qualified
            # label is "parent/base" where the parent is what distinguishes it
            # and the base is what they share, so taking the last `width` chars
            # would eat the parent and leave two rows differing only in their
            # ruined prefix ("main/foo" vs "ects/foo"). Give the parent a fixed
            # slice and the base the rest.
            if "/" in n:
                parent, base = n.split("/", 1)
                pw = max(4, width // 3)
                parent = parent[:pw]
                base = base[:max(1, width - len(parent) - 1)]
                n = parent + "/" + base
            else:
                n = n[:width]
        if n in used:                          # still colliding after trimming
            used[n] += 1
            suffix = "~%d" % used[n]
            n = n[:width - len(suffix)] + suffix
        else:
            used[n] = 1
        out[k] = n
    return out


def read_events(root, since=None):
    for path in glob.glob(os.path.join(root, "**", "*.jsonl"), recursive=True):
        # Events are appended in time order, so a file whose last write predates
        # the window can't hold one inside it. Skipping those keeps this from
        # being a full-disk read of every project you have ever opened.
        if since is not None:
            try:
                if os.path.getmtime(path) < since:
                    continue
            except OSError:
                pass
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as fh:
                for line in fh:
                    if '"usage"' not in line:
                        continue
                    try:
                        entry = json.loads(line)
                    except ValueError:
                        continue
                    msg = entry.get("message") or {}
                    usage = msg.get("usage") or {}
                    ts = _parse_ts(entry.get("timestamp"))
                    if not usage or ts is None:
                        continue
                    tokens = sum(usage.get(k, 0) or 0 for k in (
                        "input_tokens", "output_tokens",
                        "cache_creation_input_tokens", "cache_read_input_tokens"))
                    model = (msg.get("model") or "").lower()
                    yield ts, model, tokens, _project_key(entry, path, root)
        except OSError:
            continue


def get_log_windows(limits):
    root = os.path.join(os.path.expanduser("~"), ".claude", "projects")
    if not os.path.isdir(root):
        return None
    now = time.time()
    events = list(read_events(root, since=now - SEVEN_DAYS))

    def win(seconds, key, label, subset=None):
        cutoff, total, oldest = now - seconds, 0, None
        for ts, model, tok, _proj in events:
            if ts >= cutoff and (subset is None or subset in model):
                total += tok
                oldest = ts if oldest is None else min(oldest, ts)
        iso = (datetime.datetime.fromtimestamp(oldest + seconds,
               datetime.timezone.utc).isoformat() if oldest else None)
        return {"key": key, "label": label,
                "utilization": round(min(100.0, 100.0 * total / max(1, limits[key])), 1),
                "resets_at": iso}

    windows = [win(FIVE_HOURS, "five_hour", "Session (5 hour)"),
               win(SEVEN_DAYS, "seven_day", "Weekly (all models)")]
    opus = win(SEVEN_DAYS, "seven_day_opus", "Weekly (Opus)", subset="opus")
    if opus["utilization"] > 0:
        windows.append(opus)
    return windows


# ------------------------------------------------ where the tokens actually go
#
# Anthropic's usage endpoint reports account-wide windows with no per-project
# breakdown, so "which project ate my week" can only be answered from Claude
# Code's own session logs — which exist on *this* computer only. The board
# labels the screen accordingly; work done from another machine is invisible
# here, and quietly under-reporting would be worse than saying so.
#
# Shares are percentages of the window's measured tokens, not of a plan limit.
# That sidesteps the estimated-budget problem entirely: the same measured
# counts go into every project, so the ranking holds even where an absolute
# percent-of-limit would be a guess.

PROJECT_WINDOW_SECS = FIVE_HOURS
PROJECT_WINDOW_LABEL = "5h"
MAX_PROJECTS = 5


def get_project_shares(seconds=PROJECT_WINDOW_SECS, top=MAX_PROJECTS):
    """Rank this computer's projects by tokens spent in the trailing window.

    Returns (ranked, hidden) where `hidden` is how many projects fell outside
    the top N. Shares are of the whole window, so the visible ones won't add
    up to 100% when anything is hidden — the board says "+N more" rather than
    letting the missing percentage read as a rounding error."""
    root = os.path.join(os.path.expanduser("~"), ".claude", "projects")
    if not os.path.isdir(root):
        return [], 0
    cutoff = time.time() - seconds
    totals = {}
    for ts, _model, tok, key in read_events(root, since=cutoff):
        if ts >= cutoff and tok:
            totals[key] = totals.get(key, 0) + tok
    grand = sum(totals.values())
    if not grand:
        return [], 0
    totals = _roll_up_nested(totals)
    ranked = sorted(totals.items(), key=lambda kv: kv[1], reverse=True)
    labels = _label_projects([k for k, _ in ranked[:top]])
    shown = [{"name": labels[k], "share": round(100.0 * tok / grand, 1)}
             for k, tok in ranked[:top]]
    return shown, max(0, len(ranked) - len(shown))


# ------------------------------------------------------------------- push loop

def push(pi_url, token, payload):
    body = json.dumps(payload).encode("utf-8")
    headers = {"Content-Type": "application/json"}
    if token:
        headers["X-Push-Token"] = token
    req = urllib.request.Request(pi_url.rstrip("/") + "/api/push",
                                 data=body, headers=headers, method="POST")
    with urllib.request.urlopen(req, timeout=15) as resp:
        return json.loads(resp.read().decode("utf-8"))


def _pair_hmac(code, msg):
    """HMAC-SHA256(code, msg) as lowercase hex — must match the firmware."""
    return hmac.new(code.encode(), msg, hashlib.sha256).hexdigest()


def _pair_post(url, path, data=None, headers=None, timeout=15):
    req = urllib.request.Request(url.rstrip("/") + path, data=data,
                                 headers=headers or {}, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        # The board answers a refusal with a status code AND a JSON reason, and
        # urllib turns the status into an exception before anyone reads the
        # reason. Handing back the body lets callers say "the code expired"
        # instead of "couldn't reach the board", which is what it looked like.
        try:
            body = json.loads(exc.read().decode("utf-8"))
        except Exception:  # noqa: BLE001 - not JSON: a real transport failure
            raise exc
        if isinstance(body, dict):
            return body
        raise


def pair_device(url, token="", code=None, ask_code=None):
    """Hand this computer's existing Claude login to a board (Yoyu) so
    it can poll usage on its own — the user never copies a token by hand.

    The token is only sent after we prove the endpoint is the real board: it
    shows a one-time code on its screen, and we HMAC-challenge it with that code
    before transmitting anything. A discovery-race impostor never learns the
    code, so it never receives the login. If a push token is configured, that
    authorizes pairing directly instead.
    """
    creds, _ = read_creds()
    if not creds:
        # Pairing hands the board THIS computer's login, so there is nothing to
        # pair with. Say that, rather than letting the board's "login expired"
        # and the companion's silence look like two unrelated faults.
        print("Can't pair: there's no Claude Code login on this computer to "
              "give the board.", file=sys.stderr)
        _print_no_claude(board_shares_account=True)
        return False
    # The refresh token is deliberately NOT sent. It rotates, Claude Code on
    # this computer holds the same one, and a board that refreshed was
    # invalidating this computer's login roughly daily -- the board won that
    # race about half the time, and every win signed the owner out. Sharing one
    # Claude account is the normal case (a second plan costs several times what
    # the board does), so the board gets a credential it cannot rotate and the
    # companion tops it up. Firmware from v1.6.2 and earlier will still accept
    # and store a refresh token, so old boards keep the old behaviour until they
    # update -- which is the reason to update.
    oauth = {
        "accessToken": creds["accessToken"],
        "expiresAt": creds.get("expiresAt", 0),
        "subscriptionType": creds.get("subscriptionType"),
    }
    body = json.dumps(oauth).encode("utf-8")
    headers = {"Content-Type": "application/json"}

    if token:
        headers["X-Push-Token"] = token
    else:
        # 1. Ask the board to enter pairing mode — it shows a code on its screen.
        #
        # Skipped when a code was handed to us, because starting a session mints
        # a NEW code: doing it here would replace the very code the caller is
        # about to present, and --pair-code could never once have succeeded.
        if code is None:
            try:
                _pair_post(url, "/api/pair/start", timeout=10)
            except (urllib.error.URLError, OSError, ValueError) as exc:
                print(f"Couldn't reach the board at {url}: {exc}", file=sys.stderr)
                return False
            # 2. Get that code from the user (out-of-band — the whole point).
            prompt = "Enter the 6-character code shown on your board's screen: "
            code = ask_code() if ask_code else input(prompt)
        code = (code or "").strip().upper()
        if not code:
            print("No pairing code entered.", file=sys.stderr)
            return False
        # 3. Prove the endpoint knows the code BEFORE sending the token.
        nonce = secrets.token_hex(16)
        try:
            ch = _pair_post(url, "/api/pair/challenge", nonce.encode(),
                            {"Content-Type": "text/plain"}, timeout=10)
        except (urllib.error.URLError, OSError, ValueError) as exc:
            print(f"Couldn't reach the board at {url}: {exc}", file=sys.stderr)
            return False
        if ch.get("error") == "not pairing":
            print("The board isn't in pairing mode (a code is only good for "
                  "3 minutes).", file=sys.stderr)
            print("Run  --pair-start  to put a fresh code on its screen, then "
                  "pass that one.", file=sys.stderr)
            return False
        if not ch.get("ok") or not hmac.compare_digest(
                ch.get("mac", ""), _pair_hmac(code, nonce.encode())):
            print("That code didn't match the board. Double-check the screen and "
                  "retry — if it keeps failing, the device that answered may not "
                  "be your board.", file=sys.stderr)
            return False
        # 4. Endpoint proven. MAC the token so the board also confirms we hold
        #    the code (stops a random LAN device overwriting the login).
        nonce2 = secrets.token_hex(16)
        headers["X-Pair-Nonce"] = nonce2
        headers["X-Pair-Mac"] = _pair_hmac(code, nonce2.encode() + body)

    try:
        result = _pair_post(url, "/api/pair", body, headers, timeout=20)
    except (urllib.error.URLError, OSError, ValueError) as exc:
        print(f"Couldn't reach the board at {url}: {exc}", file=sys.stderr)
        return False
    if not result.get("ok"):
        print(f"Board rejected pairing: {result.get('error')}", file=sys.stderr)
        return False
    live = result.get("live")
    key = result.get("topup_key")
    if key:
        save_topup_key(url, key)
    print(f"Paired {url} - the board reads your usage itself now"
          + ("." if live else " (first read pending; it will retry)."))
    if key:
        print("It keeps going for several hours with this computer off, then "
              "waits for it.")
        print("Leave the companion set to start at login and you'll never "
              "notice the gap.")
    else:
        # An older board: it stored the refresh token and will rotate it.
        print("Note: this board's firmware is older and signs itself in "
              "independently,", file=sys.stderr)
        print("which can log Claude Code out on this computer about once a "
              "day. Updating", file=sys.stderr)
        print("the board's firmware (/update) fixes that.", file=sys.stderr)
    return True


def _config_path():
    return os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "companion.config.json")


# ------------------------------------------------------ auto-discover the Pi

def _probe_info(url, timeout=0.8):
    """The board's own /api/status, or None if whatever answered isn't a board.

    Discovery needs more than yes/no now. With two boards on one network the
    caller has to be able to tell which is which, and the only thing that says
    so is in the status body -- not in the address, which moves, and not in the
    mDNS name, which is handed out in boot order.
    """
    try:
        req = urllib.request.Request(url.rstrip("/") + "/api/status")
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            data = json.loads(resp.read().decode("utf-8"))
    except Exception:
        return None
    if data.get("app") not in (APP_MARKER,) + LEGACY_MARKERS:
        return None
    return data


def _probe(url):
    return _probe_info(url) is not None


_ID_CACHE = {}


def board_id(url, refresh=False):
    """This board's stable name. None if it can't be reached, or if it runs
    firmware older than 1.7.0, which had no id to give.

    Only answers are cached, never the absence of one. A board that happens to
    be rebooting when it is first asked would otherwise be treated as having no
    id for the rest of the run -- which is not a hypothetical: it is what
    happened the first time this was pointed at two freshly flashed boards.
    """
    u = url.rstrip("/")
    if not refresh and _ID_CACHE.get(u):
        return _ID_CACHE[u]
    ident = (_probe_info(u, timeout=3) or {}).get("id")
    if ident:
        _ID_CACHE[u] = ident
    return ident


def _local_prefixes():
    """Every /24 this computer sits on. Covers machines with more than one
    adapter (Wi-Fi + Ethernet, VPNs, Hyper-V/WSL) where the board may be on a
    different interface than the default route."""
    prefixes = []

    def add(ip):
        if (ip and ip.count(".") == 3 and not ip.startswith("127.")
                and not ip.startswith("169.254.")):
            p = ip.rsplit(".", 1)[0]
            if p not in prefixes:
                prefixes.append(p)

    try:  # the interface used to reach the internet (first, most likely)
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        add(s.getsockname()[0])
        s.close()
    except OSError:
        pass
    try:  # every other IPv4 the host knows about
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            add(info[4][0])
    except OSError:
        pass
    return prefixes


def discover_pi(port=8080):
    """Find the tracker on the LAN with no address typing. Returns URL or None."""
    # yoyu.local first, then the pre-rename hostnames so an older board on the
    # LAN is still found without the user knowing anything changed.
    for host in ("yoyu.local", "headroom.local", "claudetracker.local",
                 "claudecounter.local"):
        url = f"http://{host}:{port}"
        if _probe(url):
            return url
    found = discover_all(port)
    return found[0]["url"] if found else None


def discover_all(port=8080, quiet=False):
    """Every board on the LAN, rather than the first one that answers.

    discover_pi() stops at the first hit, which was right when there was one
    board and is wrong now that two ship. The second board was never found and
    so was never fed -- it just sat there saying it was waiting for your
    computer. Each board comes back with its own status so callers can tell
    them apart without asking again.
    """
    prefixes = _local_prefixes()
    if not prefixes:
        return []
    urls = [f"http://{p}.{i}:{port}" for p in prefixes for i in range(1, 255)]
    if not quiet:
        print("Looking for your boards on the network...")
    found = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=64) as ex:
        futures = {ex.submit(_probe_info, u): u for u in urls}
        for fut in concurrent.futures.as_completed(futures):
            try:
                info = fut.result()
            except Exception:
                continue
            if info:
                found.append({"url": futures[fut], "id": info.get("id"),
                              "board": info.get("board") or "board",
                              "version": info.get("version") or "?"})
    def _octets(b):
        try:
            return [int(x) for x in
                    b["url"].split("//")[1].split(":")[0].split(".")]
        except ValueError:
            return [0]
    found.sort(key=_octets)
    for b in found:
        _ID_CACHE[b["url"].rstrip("/")] = b["id"]
    return found


def describe_board(b):
    return "%s (%s, v%s) at %s" % (b["id"] or "unidentified", b["board"],
                                   b["version"], b["url"])


def resolve_targets(saved, rescan=False):
    """The boards to feed, re-discovering when what was saved has gone stale.

    A saved address that no longer answers used to mean pushing into the void
    forever: the value was truthy, so discovery never ran a second time. That
    is exactly what a rename or a DHCP move leaves behind -- a config still
    naming headroom.local while two boards sit on the network unfed.
    """
    targets = [t.strip() for t in str(saved or "").split(",") if t.strip()]
    if targets and not rescan and any(_probe(t) for t in targets):
        return ",".join(targets)
    found = discover_all()
    if not found:
        return ",".join(targets)      # keep what we had; it may just be off
    urls = ",".join(b["url"] for b in found)
    for b in found:
        print("Found " + describe_board(b))
    if len(found) > 1:
        print("Feeding all %d. They stay in the config, so this only has to "
              "be found once." % len(found))
    save_pi(urls)
    return urls


def disconnect_board(url, token=""):
    """Clear a board's Claude login, and forget our key for it.

    The board revokes its own top-up key when it disconnects, so a key left
    lying here is dead weight that would only produce a confusing refusal on
    the next cycle. Both ends forget together or neither does.
    """
    headers = {"Content-Type": "application/x-www-form-urlencoded"}
    if token:
        headers["X-Push-Token"] = token
    try:
        req = urllib.request.Request(url.rstrip("/") + "/disconnect", data=b"",
                                     headers=headers, method="POST")
        with urllib.request.urlopen(req, timeout=15) as resp:
            ok = resp.status == 200
    except urllib.error.HTTPError as exc:
        print("The board refused: HTTP %s%s" % (
            exc.code, " (needs the device token: --token)" if exc.code == 401 else ""),
            file=sys.stderr)
        return False
    except (urllib.error.URLError, OSError) as exc:
        print("Couldn't reach the board at %s: %s" % (url, exc), file=sys.stderr)
        return False
    drop_topup_key(url)
    _ID_CACHE.pop(url.rstrip("/"), None)
    print("Disconnected %s. Its Claude login is cleared and the top-up key is "
          "revoked; pairing again issues a new one." % url)
    return ok


def _merge_config(**fields):
    path = _config_path()
    data = {}
    if os.path.isfile(path):
        try:
            with open(path, encoding="utf-8") as fh:
                data = json.load(fh)
        except (OSError, ValueError):
            data = {}
    data.update(fields)
    try:
        with open(path, "w", encoding="utf-8") as fh:
            json.dump(data, fh, indent=2)
    except OSError:
        pass


def save_pi(url):
    _merge_config(pi=url)


def _topup_keys():
    path = _config_path()
    try:
        with open(path, encoding="utf-8") as fh:
            return json.load(fh).get("topup_keys") or {}
    except (OSError, ValueError):
        return {}


def save_topup_key(url, key):
    """Filed under the board's own id rather than its address.

    It used to be keyed by whatever URL you happened to pair against. With one
    board that was the same thing. With two it is not: yoyu.local names
    whichever board booted first, so one board's key could be looked up for the
    other, and DHCP moves the addresses underneath both.
    """
    ident = board_id(url) or url.rstrip("/")
    keys = _topup_keys()
    keys[ident] = key
    _merge_config(topup_keys=keys)


def drop_topup_key(url):
    keys = _topup_keys()
    for k in (board_id(url), url.rstrip("/")):
        if k and keys.pop(k, None) is not None:
            _merge_config(topup_keys=keys)


def _url_ip(url):
    """The address a URL actually reaches.

    Two spellings of one board have to match: a key paired against
    yoyu.local belongs to the board now sitting at 192.168.0.77, and looking it
    up by the address discovery found would otherwise miss it and quietly stop
    topping that board up.
    """
    try:
        host = urllib.parse.urlsplit(url).hostname
        return socket.gethostbyname(host) if host else None
    except (OSError, ValueError):
        return None


def topup_key_for(url):
    """This board's key: by id, then by address, then by where that address
    actually points. The last one is what carries a key written before the
    board had an id -- older firmware has none to give."""
    keys = _topup_keys()
    ident = board_id(url)
    if ident and ident in keys:
        return keys[ident]
    exact = keys.get(url.rstrip("/"))
    if exact:
        return exact
    ip = _url_ip(url)
    if not ip:
        return None
    for k, v in keys.items():
        if k.startswith("http") and _url_ip(k) == ip:
            return v
    return None


def migrate_topup_keys():
    """Re-file address-keyed keys under the board's own id.

    Runs once at startup and does nothing afterwards. A key whose address no
    longer resolves is left exactly where it is rather than dropped -- that
    board may simply be switched off, and the key is the only thing standing
    between it and having to be paired again. Guessing the wrong board is safe
    besides: the board checks the key before it reads the body, so a key
    offered to the wrong one is refused without a token ever being looked at.
    """
    keys = _topup_keys()
    moved = {}
    for k, v in list(keys.items()):
        if not k.startswith("http"):
            continue
        ident = board_id(k)
        if ident and ident != k:
            moved[ident] = v
            keys.pop(k)
            print("Filed the top-up key for %s under the board's own name (%s)."
                  % (k, ident))
    if moved:
        keys.update(moved)
        _merge_config(topup_keys=keys)
    return moved


_last_topup = {}
TOPUP_EVERY = 1800          # seconds; the access token lasts hours, not minutes


def maybe_top_up(url, now=None):
    """Rate-limited top-up. Returns True if one was actually sent."""
    now = time.time() if now is None else now
    if not topup_key_for(url):
        return False                      # not a board we paired
    if now - _last_topup.get(url, 0) < TOPUP_EVERY:
        return False
    _last_topup[url] = now
    return top_up_token(url)


def top_up_token(url, key=None):
    """Hand a board a newer access token for the account it already runs on.

    This is what replaces the board refreshing for itself. Refresh tokens
    rotate, and Claude Code on this computer holds the same one -- a board that
    refreshed was invalidating this computer's login roughly daily, which is a
    much worse bug than a board that goes stale while the computer is off.

    So the refresh stays here, where it always was, and the board is handed the
    short-lived result. Returns True if the board took it.
    """
    key = key or topup_key_for(url)
    if not key:
        return False                      # never paired from this computer
    creds, save_fn = read_creds()
    if not creds:
        return False
    token = valid_token(creds, save_fn)    # refreshes here, and writes back
    if not token:
        return False
    body = json.dumps({
        "accessToken": token,
        "expiresAt": creds.get("expiresAt", 0),
        "subscriptionType": creds.get("subscriptionType"),
    }).encode("utf-8")
    try:
        res = _pair_post(url, "/api/token", body,
                         {"Content-Type": "application/json",
                          "X-Topup-Key": key}, timeout=20)
    except (urllib.error.URLError, OSError, ValueError):
        return False
    if not res.get("ok") and "top-up key" in str(res.get("error", "")):
        # Wrong board, or the board was disconnected and minted a new key.
        # Retrying this one forever cannot fix either.
        drop_topup_key(url)
        print(f"{url} won't accept this computer's top-up key. Pair it again "
              "(tray: Pair board) -- until then it runs on what it already has.",
              file=sys.stderr)
        return False
    return bool(res.get("ok"))


# --------------------------------------------------------- install and login

INSTALLED_MARKER = os.path.expanduser("~/.claudetracker-companion-installed")

APP_NAME = "Yoyu Companion"


def install_dir():
    """Where the app lives once installed.

    Per-user, so nothing needs an administrator. On Windows that is the same
    place winget and VS Code put user installs; on Linux it follows the XDG
    convention for a binary you did not get from a package manager.
    """
    if sys.platform == "win32":
        base = os.environ.get("LOCALAPPDATA") or os.path.expanduser("~")
        return os.path.join(base, "Programs", "Yoyu")
    if sys.platform == "darwin":
        return os.path.expanduser("~/Library/Application Support/Yoyu")
    return os.path.expanduser("~/.local/share/yoyu")


def installed_exe():
    name = "YoyuCompanion.exe" if sys.platform == "win32" else "YoyuCompanion"
    return os.path.join(install_dir(), name)


def is_installed():
    return os.path.isfile(installed_exe())


def sweep_stale_install():
    """Delete the copy an upgrade had to move aside.

    Windows will not let a running .exe be overwritten, so install_app renames
    the old one to .old and carries on. Nothing has it open by the time the new
    copy starts, but it is 8MB and it used to sit there until the next install,
    which might be never.
    """
    stale = installed_exe() + ".old"
    try:
        if os.path.isfile(stale):
            os.remove(stale)
            return stale
    except OSError:
        pass          # still locked; the next start will get it
    return None


def running_from_install():
    return (getattr(sys, "frozen", False)
            and os.path.abspath(sys.executable) == installed_exe())


def _launch_argv():
    """How to relaunch the companion at login.

    Prefers the installed copy over whichever file is running right now. That
    distinction is the whole point of installing: autostart used to record
    sys.executable, so a binary run straight out of the Downloads folder
    registered *that* path, and emptying Downloads silently broke start-up
    with nothing to see and nothing to fix.
    """
    if is_installed():
        return [installed_exe()]
    if getattr(sys, "frozen", False):
        return [os.path.abspath(sys.executable)]
    return [sys.executable or "python3", os.path.abspath(__file__)]


def _win_shortcut(link_path, target):
    """A real .lnk, via the shell's own COM object.

    A .cmd shim in the Start Menu would work and need no dependencies, but it
    flashes a console window on every launch and shows up with the wrong icon.
    Falls back to the shim if PowerShell is unavailable or refuses.
    """
    ps = ("$s = (New-Object -ComObject WScript.Shell).CreateShortcut(%r); "
          "$s.TargetPath = %r; $s.WorkingDirectory = %r; $s.Save()"
          % (link_path, target, os.path.dirname(target)))
    try:
        r = subprocess.run(["powershell", "-NoProfile", "-NonInteractive",
                            "-Command", ps], capture_output=True, timeout=30)
        if r.returncode == 0 and os.path.exists(link_path):
            return link_path
    except (OSError, subprocess.SubprocessError):
        pass
    shim = os.path.splitext(link_path)[0] + ".cmd"
    with open(shim, "w", encoding="utf-8") as fh:
        fh.write('@echo off\r\nstart "" "%s"\r\n' % target)
    return shim


def install_app(startup=True):
    """Copy the app somewhere stable and give it a launcher entry.

    Returns a list of human-readable lines describing what was done, so the
    caller can print it or show it. Raises on anything that actually failed.
    """
    if not getattr(sys, "frozen", False):
        raise RuntimeError(
            "Running from source, so there is no single file to install. "
            "Use --startup on its own to add the login item, pointing at this "
            "checkout.")
    done = []
    src = os.path.abspath(sys.executable)
    dst = installed_exe()
    os.makedirs(install_dir(), exist_ok=True)
    if os.path.abspath(src) != dst:
        # Windows will not let a running .exe be overwritten, and an upgrade in
        # place is exactly when that happens. Move the old one aside first; it
        # can be deleted on the next run, once nothing has it open.
        if os.path.exists(dst):
            stale = dst + ".old"
            try:
                if os.path.exists(stale):
                    os.remove(stale)
                os.replace(dst, stale)
            except OSError:
                pass
        shutil.copy2(src, dst)
        try:
            os.chmod(dst, 0o755)
        except OSError:
            pass
        done.append("Installed to %s" % dst)
    else:
        done.append("Already installed at %s" % dst)

    # A launcher entry, so it can be started like any other app rather than by
    # finding the file again.
    if sys.platform == "win32":
        menu = os.path.join(os.environ.get("APPDATA", ""), "Microsoft",
                            "Windows", "Start Menu", "Programs")
        os.makedirs(menu, exist_ok=True)
        done.append("Start Menu: %s"
                    % _win_shortcut(os.path.join(menu, APP_NAME + ".lnk"), dst))
    elif sys.platform != "darwin":
        apps = os.path.expanduser("~/.local/share/applications")
        os.makedirs(apps, exist_ok=True)
        desktop = os.path.join(apps, "yoyu-companion.desktop")
        with open(desktop, "w", encoding="utf-8") as fh:
            fh.write("[Desktop Entry]\n"
                     "Type=Application\n"
                     "Name=%s\n"
                     "Comment=Feed your Claude usage to a Yoyu board\n"
                     "Exec=%s\n"
                     "Terminal=false\n"
                     "Categories=Utility;\n" % (APP_NAME, dst))
        done.append("Menu entry: %s" % desktop)
        # A path entry too, so `yoyu-companion` works in a shell.
        bindir = os.path.expanduser("~/.local/bin")
        os.makedirs(bindir, exist_ok=True)
        link = os.path.join(bindir, "yoyu-companion")
        try:
            if os.path.islink(link) or os.path.exists(link):
                os.remove(link)
            os.symlink(dst, link)
            done.append("Command: %s" % link)
        except OSError:
            pass

    if startup:
        done.append("Starts at login: %s" % install_autostart())
    else:
        done.append("Not starting at login (add it later with --startup)")
    return done


def uninstall_app():
    """Undo install_app. Leaves the config and the paired keys alone."""
    removed = uninstall_autostart()
    targets = [installed_exe(), installed_exe() + ".old"]
    if sys.platform == "win32":
        menu = os.path.join(os.environ.get("APPDATA", ""), "Microsoft",
                            "Windows", "Start Menu", "Programs")
        targets += [os.path.join(menu, APP_NAME + ".lnk"),
                    os.path.join(menu, APP_NAME + ".cmd")]
    elif sys.platform != "darwin":
        targets += [os.path.expanduser("~/.local/share/applications/"
                                       "yoyu-companion.desktop"),
                    os.path.expanduser("~/.local/bin/yoyu-companion")]
    for t in targets:
        try:
            if os.path.islink(t) or os.path.isfile(t):
                # Do not delete the file we are currently running from; on
                # Windows that fails outright, and elsewhere it would leave the
                # user with no app and no warning.
                if getattr(sys, "frozen", False) and \
                        os.path.abspath(t) == os.path.abspath(sys.executable):
                    continue
                os.remove(t)
                removed.append(t)
        except OSError:
            pass
    try:
        if os.path.isdir(install_dir()) and not os.listdir(install_dir()):
            os.rmdir(install_dir())
    except OSError:
        pass
    return removed


def install_autostart():
    """Set the companion to launch at login. Returns a human-readable path."""
    argv = _launch_argv()
    if sys.platform == "win32":
        # Frozen: run the exe directly. From source: prefer pythonw (no console).
        if not getattr(sys, "frozen", False) and argv[0].lower().endswith("python.exe"):
            argv[0] = argv[0][:-len("python.exe")] + "pythonw.exe"
        cmd = " ".join(f'"{a}"' for a in argv)
        startup = os.path.join(os.environ.get("APPDATA", ""), "Microsoft",
                               "Windows", "Start Menu", "Programs", "Startup")
        os.makedirs(startup, exist_ok=True)
        target = os.path.join(startup, _WIN_AUTOSTART_NAMES[0])
        with open(target, "w", encoding="utf-8") as fh:
            fh.write(f'@echo off\r\nstart "" {cmd}\r\n')
        return target
    if sys.platform == "darwin":
        d = os.path.expanduser("~/Library/LaunchAgents")
        os.makedirs(d, exist_ok=True)
        target = os.path.join(d, "com.claudetracker.companion.plist")
        args_xml = "".join(f"<string>{a}</string>" for a in argv)
        with open(target, "w", encoding="utf-8") as fh:
            fh.write(f"""<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
 "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>Label</key><string>com.claudetracker.companion</string>
  <key>ProgramArguments</key>
  <array>{args_xml}</array>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
</dict></plist>""")
        subprocess.run(["launchctl", "unload", target],
                       capture_output=True)
        subprocess.run(["launchctl", "load", target], capture_output=True)
        return target
    # linux
    d = os.path.expanduser("~/.config/systemd/user")
    os.makedirs(d, exist_ok=True)
    target = os.path.join(d, "claudetracker-companion.service")
    exec_start = " ".join(argv)
    with open(target, "w", encoding="utf-8") as fh:
        fh.write(f"""[Unit]
Description=Yoyu companion
After=network-online.target

[Service]
ExecStart={exec_start}
Restart=always
RestartSec=30

[Install]
WantedBy=default.target
""")
    subprocess.run(["systemctl", "--user", "enable", "--now",
                    "claudetracker-companion"], capture_output=True)
    return target


# Every Windows autostart filename this project has ever written. The product
# has been renamed twice, install_autostart() only ever writes the current name,
# and an entry it does not know about keeps launching a companion at every login
# forever. Found in the wild: a machine still had ClaudeTrackerCompanion.bat from
# two names ago, quietly starting the companion after --uninstall reported
# success, because the previous fix for exactly this bug only went back one
# generation. Append here, never replace.
_WIN_AUTOSTART_NAMES = (
    "YoyuCompanion.bat",              # current
    "HeadroomCompanion.bat",          # Headroom Mini
    "ClaudeTrackerCompanion.bat",     # the original
)


def _win_startup_dir():
    return os.path.join(os.environ.get("APPDATA", ""), "Microsoft", "Windows",
                        "Start Menu", "Programs", "Startup")


def sweep_stale_autostart(startup_dir=None):
    """Delete Windows Startup entries this project wrote under an older name.

    --uninstall only helps people who run it. Everyone who upgraded through an
    earlier name still has a stale entry launching a companion at every login,
    polling Anthropic on the same account as everything else -- and two pollers
    on one account is what rate-limits a board into showing nothing. They have
    no way to know that, so the fix cannot be a command they have to find.

    Deliberately conservative about what it deletes, because this is somebody
    else's Startup folder:
      * only names this project has written, never the current one;
      * only if the file actually launches a companion, so an unrelated file
        that happens to share the name survives;
      * and it says what it removed rather than doing it silently.
    """
    if os.name != "nt":
        return []               # the macOS and Linux unit names never changed
    d = startup_dir or _win_startup_dir()
    removed = []
    for name in _WIN_AUTOSTART_NAMES[1:]:      # [0] is the current name: keep it
        path = os.path.join(d, name)
        if not os.path.isfile(path):
            continue
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as fh:
                body = fh.read()
        except OSError:
            continue
        if "companion" not in body.lower():
            continue            # same name, not ours -- leave it alone
        try:
            os.remove(path)
            removed.append(path)
        except OSError:
            pass
    return removed


def uninstall_autostart():
    removed = []
    paths = [os.path.join(_win_startup_dir(), n) for n in _WIN_AUTOSTART_NAMES]
    paths += [
        # The macOS and Linux unit names never changed, so there is only ever
        # one of each to remove.
        os.path.expanduser("~/Library/LaunchAgents/"
                           "com.claudetracker.companion.plist"),
        os.path.expanduser("~/.config/systemd/user/"
                           "claudetracker-companion.service"),
    ]
    for p in paths:
        if os.path.isfile(p):
            try:
                os.remove(p)
                removed.append(p)
            except OSError:
                pass
    for m in (INSTALLED_MARKER,):
        if os.path.isfile(m):
            os.remove(m)
    return removed


def load_config():
    cfg = {"pi": None, "token": "", "interval": 120, "plan": "max"}
    data = {}
    path = _config_path()
    if os.path.isfile(path):
        try:
            with open(path, encoding="utf-8") as fh:
                data = json.load(fh)
            cfg.update({k: data[k] for k in ("pi", "token", "interval")
                        if k in data})
            if data.get("plan") in PLAN_PRESETS:
                cfg["plan"] = data["plan"]
            # Optional: remap what the board's Actions screen types, e.g.
            # {"action_keys": {"cancel": "ctrl+c"}}
            if isinstance(data.get("action_keys"), dict):
                cfg["action_keys"] = {str(k): str(v) for k, v
                                      in data["action_keys"].items()}
        except (OSError, ValueError) as exc:
            print(f"Ignoring bad companion.config.json: {exc}", file=sys.stderr)
    # Estimation budgets: start from the plan preset, then apply any overrides.
    cfg["limits"] = dict(PLAN_PRESETS[cfg["plan"]])
    if isinstance(data.get("limits"), dict):
        cfg["limits"].update(data["limits"])
    return cfg


def login_state(claude_dir=None):
    """Which of the several 'no login' situations we are actually in.

    These used to collapse into one message that said "you're not signed in",
    which reads as simply wrong to somebody whose Claude Code is working fine
    in the next window. The states need different things done about them, and
    "signed_out" in particular has a cause worth naming -- see login_help().

    claude_dir is for tests; production always reads the real location.
    """
    claude_dir = claude_dir or os.path.join(os.path.expanduser("~"), ".claude")
    if not os.path.isdir(claude_dir):
        return "not_installed"
    path = os.path.join(claude_dir, ".credentials.json")
    if not os.path.isfile(path):
        return "never_signed_in"
    try:
        with open(path, "r", encoding="utf-8") as fh:
            raw = json.load(fh)
    except (OSError, ValueError):
        return "unreadable"
    o = raw.get("claudeAiOauth") if isinstance(raw, dict) else None
    o = o if isinstance(o, dict) else (raw if isinstance(raw, dict) else {})
    if o.get("accessToken") or o.get("access_token"):
        return "ok"
    # The keys are present and empty rather than missing: something signed
    # Claude Code out and wrote the record back without its secrets. That is a
    # different situation from never having logged in, and it is the one a
    # second device sharing the account produces.
    if "accessToken" in o or "access_token" in o:
        return "signed_out"
    return "never_signed_in"


def login_help(state=None, board_shares_account=False):
    """The lines to show someone whose Claude Code login can't be read.

    Returned rather than printed so the tray app -- which has no console -- can
    put the same words in front of people.
    """
    state = state or login_state()
    out = []
    if state == "not_installed":
        out += ["Claude Code isn't installed on this computer.",
                "",
                "  Yoyu reads Claude Code's own login to get your real usage",
                "  numbers, so it needs Claude Code installed and signed in on",
                "  the machine you actually work on.",
                "",
                "  Install it from  https://claude.com/claude-code  , then run",
                "  this again."]
        return out

    if state == "signed_out":
        out += ["Your Claude Code login on this computer has been cleared.",
                "",
                "  The login file is still there, but its tokens are empty --",
                "  something signed Claude Code out. Signing in again fixes it."]
    elif state == "unreadable":
        out += ["Your Claude Code login file couldn't be read.",
                "",
                "  It exists but isn't valid JSON. Signing in again rewrites it."]
    else:
        out += ["No Claude Code login found on this computer.",
                "",
                "  Claude Code is installed here but hasn't been signed in yet."]

    out += ["",
            "  To fix it:",
            "    1. Open a terminal.",
            "    2. Run:   claude",
            "    3. Type:  /login      and follow the browser prompt.",
            "    4. Close it, then start Yoyu again.",
            "",
            "  It takes about a minute, and you only see the browser once."]

    if board_shares_account or state == "signed_out":
        out += ["",
                "  If your Yoyu board is signed in to this SAME Claude account,",
                "  expect this to come back: the board and this computer refresh",
                "  the same login and rotate each other out, usually within a day.",
                "  Give the board its own Claude login, or stop it polling and",
                "  leave the companion to feed it."]
    return out


# Console output stays ASCII on purpose. Windows terminals default to cp1252,
# which cannot encode an em dash, so one printed at the moment of success came
# out as "Paired http://yoyu.local:8080 ? the board reads your usage itself now"
# -- a garbled character in the first thing a new user is told.
def _print_no_claude(state=None, board_shares_account=False):
    """A clear, actionable message when there's no Claude Code login to read."""
    print("", file=sys.stderr)
    for line in login_help(state, board_shares_account):
        print(line, file=sys.stderr)
        print("  Install:  npm install -g @anthropic-ai/claude-code",
              file=sys.stderr)
        print("  then run  claude  and type  /login .", file=sys.stderr)
    print("  (Run this companion on the same computer where you use Claude "
          "Code - not on the Pi.)", file=sys.stderr)


def run_once(cfg):
    """One poll+push cycle. Returns (ok, retry_after_seconds, rate_limited)."""
    try:
        live = get_live_windows()
    except LiveUnavailable as exc:
        # A login exists but live usage is temporarily unreadable (rate
        # limit, network blip). Skip this push: the tracker keeps showing
        # the last REAL numbers instead of wrong log-based estimates.
        print(f"live usage unavailable ({exc}); skipping this push so the "
              "tracker keeps its last real reading", file=sys.stderr)
        return False, min(900, exc.retry_after), exc.rate_limited
    credits = None
    if live:
        windows, plan, credits = live
        source = "live"
    else:
        # No Claude Code login on this machine at all -> estimation is the
        # best we can do (clearly tagged as such).
        windows = get_log_windows(cfg["limits"])
        plan, source = None, "estimated"
        if not windows:
            _print_no_claude()
            return False, 0, False
    payload = {"windows": windows, "plan": plan, "source": source}
    # Project shares ride along on both paths. They come from the local logs
    # even when the windows above are live, because the live endpoint has no
    # per-project breakdown to offer — the two answer different questions and
    # are not expected to agree.
    try:
        projects, hidden = get_project_shares()
    except OSError as exc:                       # unreadable logs shouldn't
        projects, hidden = [], 0                 # cost you the usage push
        print(f"(couldn't read project usage: {exc})", file=sys.stderr)
    # Sent unconditionally, empty included. The firmware reads an absent key as
    # "keep what you have" (so an older companion doesn't blank the screen), so
    # omitting it on a quiet window would leave this morning's ranking on
    # display under a caption claiming it covers the last 5 hours.
    # Beside the windows, never among them -- see credits_from_usage().
    if credits is not None:
        payload["credits"] = credits
    payload["projects"] = projects
    payload["projects_window"] = PROJECT_WINDOW_LABEL
    payload["projects_more"] = hidden
    # cfg["pi"] may be a comma-separated list — one companion can feed
    # several trackers (e.g. a Pi on the desk and a Mini on the shelf).
    targets = [t.strip() for t in str(cfg["pi"]).split(",") if t.strip()]
    # Top up any self-hosted board's access token while we are here. The board
    # cannot mint one for itself by design, so this is the whole reason it keeps
    # working when this computer is asleep. Cheap: it is skipped for boards we
    # never paired, and the token is one we already hold.
    for target in targets:
        maybe_top_up(target)
    delivered = 0
    for target in targets:
        try:
            result = push(target, cfg["token"], payload)
        except (urllib.error.URLError, OSError) as exc:
            print(f"Couldn't reach the tracker at {target}: {exc}",
                  file=sys.stderr)
            continue
        if result.get("ok"):
            delivered += 1
        else:
            print(f"{target} rejected the push: {result.get('error')}",
                  file=sys.stderr)
    if delivered == 0:
        return False, 0, False
    tag = "LIVE" if source == "live" else "estimated"
    summary = ", ".join(f"{w['label'].split(' (')[0]} {w['utilization']}%"
                        for w in windows)
    where = f" -> {delivered}/{len(targets)} trackers" if len(targets) > 1 else ""
    print(f"pushed [{tag}]{where}: {summary}")
    return True, 0, False


# ---- Actions: a tap on the board becomes a keystroke on this computer ------
#
# Off unless you pass --actions. Synthesising keypresses is a real capability,
# so it is never enabled behind your back. The board only ever queues an action
# when someone physically taps its screen — nothing on the network can inject
# one — but the keystroke lands in whatever window happens to be focused here,
# which is why this is opt-in.

DEFAULT_ACTION_KEYS = {
    "voice": "space",         # Claude Code voice mode
    "mode": "shift+tab",      # cycle mode
    "cancel": "escape",       # interrupt
}
ACTION_POLL_SECS = 1.0        # a button has to feel immediate
ACTION_ERROR_BACKOFF = 5.0    # board unreachable: stop hammering it


_MODIFIERS = {"shift", "ctrl", "alt", "cmd"}


def _has_real_key(parts):
    """A combo must contain something other than modifiers — 'shift' on its own
    isn't a shortcut, and silently pressing a bare modifier looks like a bug."""
    return any(p not in _MODIFIERS for p in parts)


def _send_keys_windows(combo):
    import ctypes
    VK = {"space": 0x20, "tab": 0x09, "shift": 0x10, "ctrl": 0x11,
          "alt": 0x12, "escape": 0x1B, "enter": 0x0D}
    parts = [p.strip().lower() for p in combo.split("+") if p.strip()]
    if not _has_real_key(parts):
        return False
    codes = []
    for p in parts:
        if p in VK:
            codes.append(VK[p])
        elif len(p) == 1 and p.isalnum():   # plain letters/digits: VK == ASCII
            codes.append(ord(p.upper()))
        else:
            return False
    user32 = ctypes.windll.user32
    for code in codes:                       # press in order
        user32.keybd_event(code, 0, 0, 0)
    for code in reversed(codes):             # release in reverse
        user32.keybd_event(code, 0, 2, 0)    # 2 = KEYEVENTF_KEYUP
    return True


def _send_keys_macos(combo):
    # Needs Accessibility permission for whatever runs this (Terminal, or the
    # packaged app): System Settings -> Privacy & Security -> Accessibility.
    CODE = {"space": 49, "tab": 48, "escape": 53, "enter": 36}
    MOD = {"shift": "shift down", "ctrl": "control down",
           "alt": "option down", "cmd": "command down"}
    parts = [p.strip().lower() for p in combo.split("+") if p.strip()]
    mods = [MOD[p] for p in parts if p in MOD]
    rest = [p for p in parts if p not in MOD]
    if len(rest) != 1:
        return False
    key = rest[0]
    using = f" using {{{', '.join(mods)}}}" if mods else ""
    if key in CODE:
        target = f"key code {CODE[key]}"
    elif len(key) == 1 and key.isalnum():   # plain character
        target = f'keystroke "{key}"'
    else:
        return False
    script = f'tell application "System Events" to {target}{using}'
    subprocess.run(["osascript", "-e", script], capture_output=True, timeout=5)
    return True


def _send_keys_linux(combo):
    XDO = {"space": "space", "tab": "Tab", "escape": "Escape",
           "enter": "Return", "shift": "shift", "ctrl": "ctrl", "alt": "alt"}
    parts = [p.strip().lower() for p in combo.split("+") if p.strip()]
    if not _has_real_key(parts):
        return False
    keys = []
    for p in parts:
        if p in XDO:
            keys.append(XDO[p])
        elif len(p) == 1 and p.isalnum():   # xdotool takes plain chars as-is
            keys.append(p)
        else:
            return False
    subprocess.run(["xdotool", "key", "+".join(keys)],
                   capture_output=True, timeout=5)
    return True


def send_keys(combo):
    """Type a combo like 'shift+tab' here. Returns True if it was delivered."""
    try:
        if sys.platform.startswith("win"):
            return _send_keys_windows(combo)
        if sys.platform == "darwin":
            return _send_keys_macos(combo)
        return _send_keys_linux(combo)
    except FileNotFoundError:
        print("Actions need 'xdotool' installed to send keystrokes "
              "(sudo apt install xdotool).", file=sys.stderr)
        return False
    except Exception as exc:  # noqa: BLE001
        print(f"Couldn't send keystroke: {exc}", file=sys.stderr)
        return False


def poll_actions(url, token, keymap):
    """Collect button presses from the board and replay them as keystrokes.
    Runs forever; intended for a daemon thread."""
    url = url.rstrip("/") + "/api/actions"
    headers = {"X-Push-Token": token} if token else {}
    while True:
        delay = ACTION_POLL_SECS
        try:
            req = urllib.request.Request(url, headers=headers)
            with urllib.request.urlopen(req, timeout=5) as resp:
                data = json.loads(resp.read().decode("utf-8"))
            for name in data.get("actions") or []:
                combo = keymap.get(name)
                if not combo:
                    print(f"(board asked for unknown action {name!r})",
                          file=sys.stderr)
                    continue
                if send_keys(combo):
                    print(f"action: {name} -> {combo}")
        except (urllib.error.URLError, OSError, ValueError):
            delay = ACTION_ERROR_BACKOFF     # board asleep/offline; try later
        time.sleep(delay)


LOCK_PORT = 47823   # localhost mutex so two companions can't double-poll


def _single_instance():
    """Bind a localhost port as a process-wide lock. Returns the socket to
    hold for our lifetime, or None if another companion already has it."""
    import socket as _socket
    s = _socket.socket(_socket.AF_INET, _socket.SOCK_STREAM)
    try:
        s.bind(("127.0.0.1", LOCK_PORT))
        s.listen(1)
        return s
    except OSError:
        s.close()
        return None


def main():
    cfg = load_config()
    ap = argparse.ArgumentParser(description="Yoyu companion")
    ap.add_argument("--install", action="store_true",
                    help="copy the app somewhere permanent, add a launcher "
                         "entry, and start it at login")
    ap.add_argument("--no-startup", action="store_true",
                    help="with --install: set it up but do not start at login")
    ap.add_argument("--startup", action="store_true",
                    help="add the login item on its own, without installing")
    ap.add_argument("--disconnect", nargs="?", const=True, metavar="URL",
                    help="clear a board's Claude login and forget its top-up "
                         "key (the reverse of --pair)")
    ap.add_argument("--rescan", action="store_true",
                    help="look for boards again even if the saved ones answer "
                         "-- use after adding a second board")
    ap.add_argument("--pi", default=None,
                    help="tracker URL(s), comma-separated for multiple "
                         "devices (auto-discovered if omitted)")
    ap.add_argument("--token", default=cfg["token"])
    ap.add_argument("--interval", type=int, default=cfg["interval"])
    ap.add_argument("--once", action="store_true", help="push once and exit")
    ap.add_argument("--pair", nargs="?", const="", default=None, metavar="URL",
                    help="send this computer's Claude login to a board so it "
                         "runs self-contained, then exit (board auto-found if "
                         "no URL is given). The board shows a one-time code you "
                         "confirm, so the login only goes to your real board.")
    ap.add_argument("--pair-start", nargs="?", const="", default=None,
                    metavar="URL",
                    help="put the board into pairing mode and exit, so the code "
                         "on its screen can be passed to --pair-code (for "
                         "terminals with no interactive prompt)")
    ap.add_argument("--pair-code", default=None, metavar="CODE",
                    help="the code shown on the board's screen (otherwise you "
                         "are prompted for it during --pair)")
    ap.add_argument("--actions", action="store_true",
                    help="let the board's Actions screen send keystrokes to "
                         "this computer (off by default; the keypress lands in "
                         "whatever window is focused here)")
    ap.add_argument("--no-install", action="store_true",
                    help="don't add to startup")
    ap.add_argument("--uninstall", action="store_true",
                    help="remove from startup and exit")
    args = ap.parse_args()

    if args.pair is not None:
        url = args.pair
        if not url:
            print("Looking for your board on the network...")
            url = discover_pi()
        if not url:
            ap.error("couldn't find a board on your network. Make sure it's "
                     "powered on and on the same Wi-Fi, or pass the address "
                     "shown on its screen: --pair http://<its-address>:8080")
        sys.exit(0 if pair_device(url, token=args.token,
                                  code=args.pair_code) else 1)

    if args.pair_start is not None:
        url = args.pair_start or cfg.get("pi") or discover_pi()
        if not url:
            print("Couldn't find a board. Pass one: --pair-start "
                  "http://<its-address>:8080", file=sys.stderr)
            sys.exit(1)
        try:
            _pair_post(url, "/api/pair/start", timeout=10)
        except (urllib.error.URLError, OSError, ValueError) as exc:
            print(f"Couldn't reach the board at {url}: {exc}", file=sys.stderr)
            sys.exit(1)
        print(f"{url} is showing a 6-character code on its screen, good for "
              "3 minutes.")
        print(f"Finish with:  companion.py --pair {url} --pair-code <CODE>")
        return

    if args.disconnect:
        url = args.disconnect if isinstance(args.disconnect, str) else (
            cfg.get("pi") or "").split(",")[0].strip() or discover_pi()
        if not url:
            ap.error("couldn't find a board to disconnect; pass "
                     "--disconnect http://<its-address>:8080")
        sys.exit(0 if disconnect_board(url, args.token or cfg.get("token", "")) else 1)

    if args.install:
        try:
            for line in install_app(startup=not args.no_startup):
                print(line)
        except Exception as exc:  # noqa: BLE001 - report, do not traceback
            print("Install failed: %s" % exc, file=sys.stderr)
            sys.exit(1)
        if not is_installed():
            return
        print("")
        print("Run it from your applications menu, or just leave it; it will "
              "start on its own next time you log in.")
        return

    if args.startup:
        print("Starts at login: %s" % install_autostart())
        with open(INSTALLED_MARKER, "w", encoding="utf-8") as fh:
            fh.write(str(cfg.get("pi") or ""))
        return

    if args.uninstall:
        removed = uninstall_app()
        print("Removed:\n  " + "\n  ".join(removed) if removed
              else "Nothing to remove.")
        return

    saved_pi = cfg["pi"]
    cfg["pi"], cfg["token"], cfg["interval"] = args.pi, args.token, args.interval
    if not cfg["pi"]:
        # Not just "is it set" but "does it still answer": a stale saved
        # address is truthy and would otherwise never be looked at again.
        cfg["pi"] = resolve_targets(saved_pi, rescan=args.rescan)
    if not cfg["pi"]:
        ap.error("couldn't find a board on your network. Make sure it's "
                 "powered on and on the same Wi-Fi, or pass "
                 "--pi http://<its-address>:8080")
    migrate_topup_keys()
    # Before any early return below. Sitting further down meant --once never
    # reached it, so the 8MB copy an upgrade leaves behind survived every run
    # of the one mode a scheduled task is most likely to use.
    sweep_stale_install()

    if args.once:
        print(f"Yoyu companion -> {cfg['pi']} (single push)")
        ok, _, _ = run_once(cfg)
        sys.exit(0 if ok else 1)

    lock = _single_instance()
    if lock is None:
        print("Another Yoyu companion is already running on this computer "
              "(probably the auto-started one) — exiting so we don't "
              "double-poll Anthropic. To run this one instead, stop the other "
              "first (or reboot after --uninstall).")
        return

    print(f"Yoyu companion -> {cfg['pi']} (every {cfg['interval']}s)")
    if args.actions:
        # First target only: keystrokes land on this computer, so a second
        # board sending them here would just be two remotes for one keyboard.
        board = str(cfg["pi"]).split(",")[0].strip()
        keymap = dict(DEFAULT_ACTION_KEYS)
        keymap.update(cfg.get("action_keys") or {})
        threading.Thread(target=poll_actions, daemon=True,
                         args=(board, cfg["token"], keymap)).start()
        print(f"Actions enabled: taps on {board} will type here.")
    # Before anything else touches autostart: clear out entries left by older
    # names of this product, which would otherwise keep starting a second
    # companion at every login for as long as the machine lives.
    for stale in sweep_stale_autostart():
        print("Removed a leftover auto-start entry from an older version:\n"
              f"  {stale}")
    first_ok, _, _ = run_once(cfg)
    # Deliberately does NOT install itself any more. It used to add a login
    # item after the first good push without asking, pointing at whatever path
    # the binary was run from -- usually the Downloads folder, so emptying
    # Downloads broke start-up silently. Say it once instead, and let the
    # person decide.
    if first_ok and not args.no_install and not os.path.isfile(INSTALLED_MARKER) \
            and not running_from_install():
        print("")
        print("Tip: run this once with --install to keep a permanent copy and "
              "start it at login.")
        if getattr(sys, "frozen", False) and not is_installed():
            print("     Right now it will only run from %s"
                  % os.path.abspath(sys.executable))
    # When Anthropic rate-limits the usage endpoint (HTTP 429), stop hammering
    # it: back off exponentially (2x per consecutive 429, capped at 30 min) and
    # honour any Retry-After the server sends as a floor. A single good read
    # resets us to the normal cadence.
    base = max(30, cfg["interval"])
    rl_backoff = 0
    while True:
        time.sleep(base + rl_backoff)
        _ok, retry_after, rate_limited = run_once(cfg)
        if rate_limited:
            rl_backoff = min(1800, max(retry_after, rl_backoff * 2 or base))
            print(f"rate limited by Anthropic - backing off, next try in "
                  f"~{base + rl_backoff}s (staying quiet so we stop hammering "
                  "the usage endpoint)", file=sys.stderr)
        else:
            if rl_backoff:
                print("usage endpoint recovered - back to normal cadence",
                      file=sys.stderr)
            rl_backoff = 0


if __name__ == "__main__":
    main()
