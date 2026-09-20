#!/usr/bin/env python3
"""
Claude Active Sessions Manager (Playwright - Python) - Every-Minute Bot

Continuously monitors active Claude sessions and terminates any session
not located in a protected location. Runs on the locally installed Chrome
(chrome.exe), never on Playwright's bundled Chromium.

Safety model: a session is terminated only when its location is read
successfully AND is not protected. Anything unclear - blank location,
unreadable row, the current session, a table that looks wrong - is kept.
"""

import argparse
import asyncio
import datetime
import json
import os
import re
import signal
import sys
import urllib.request
from typing import List, Set, Tuple

# Locations that are ALWAYS protected. --allow only ADDS to this list; nothing
# can remove an entry. Matching is case-insensitive substring matching, so
# "Mumbai, Maharashtra, IN" and "Bergen, Vestland, NO" both match.
PROTECTED_LOCATIONS = ["mumbai", "navi mumbai", "bergen", "panvel"]

# Chrome profile used by default (folder name, display name or account email).
DEFAULT_CHROME_PROFILE = "shreyans.tatiya@gmail.com"

# Claude's account settings page, where the active-session table lives.
SETTINGS_URL = "https://claude.ai/settings/account"


def is_location_allowed(location: str, allowed_locations: List[str]) -> bool:
    if not location:
        return False
    loc_lower = location.lower()
    full_list = list(dict.fromkeys(PROTECTED_LOCATIONS + list(allowed_locations or [])))
    return any(str(allowed).lower() in loc_lower for allowed in full_list)


def classify_session(device_text: str, location_text: str, is_current: bool,
                     allowed_locations: List[str]) -> Tuple[bool, str]:
    """Decide what to do with one session row, biased towards keeping it.

    Returns (keep, reason).
    """
    # The bot's own session is always the one Claude marks "Current": that flag
    # is rendered for whichever session requested the page, i.e. this browser.
    if is_current:
        return True, "Current session"

    location = " ".join((location_text or "").split())

    # Fail-safe: a row whose location did not render must never be terminated -
    # we simply do not know where it is.
    if location in ("", "-", "--"):
        return True, "Location unknown - kept for safety"

    if is_location_allowed(location, allowed_locations):
        return True, "Protected location"

    if not (device_text or "").strip():
        return True, "Row not readable - kept for safety"

    return False, f'Location "{location}" is not protected'


def find_chrome_executable(explicit=None):
    """Locate the locally installed Chrome (not Playwright's bundled Chromium)."""
    candidates = []
    if explicit:
        candidates.append(explicit)
    if os.environ.get("CHROME_PATH"):
        candidates.append(os.environ["CHROME_PATH"])

    if sys.platform == "win32":
        for root in (os.environ.get("PROGRAMFILES"), os.environ.get("PROGRAMFILES(X86)"),
                     os.environ.get("LOCALAPPDATA")):
            if root:
                candidates.append(os.path.join(root, "Google", "Chrome", "Application", "chrome.exe"))
    elif sys.platform == "darwin":
        candidates.append("/Applications/Google Chrome.app/Contents/MacOS/Google Chrome")
    else:
        candidates += ["/usr/bin/google-chrome", "/usr/bin/google-chrome-stable",
                       "/opt/google/chrome/chrome"]

    for candidate in candidates:
        if candidate and os.path.exists(candidate):
            return candidate
    return None


def default_user_data_dir():
    """Where Chrome keeps this OS user's real profiles."""
    if sys.platform == "win32":
        return os.path.join(os.environ.get("LOCALAPPDATA", ""), "Google", "Chrome", "User Data")
    if sys.platform == "darwin":
        return os.path.expanduser("~/Library/Application Support/Google/Chrome")
    return os.path.expanduser("~/.config/google-chrome")


def list_chrome_profiles():
    """Read the profile list Chrome keeps in 'Local State'."""
    local_state = os.path.join(default_user_data_dir(), "Local State")
    profiles = []
    try:
        with open(local_state, "r", encoding="utf-8") as fh:
            state = json.load(fh)
        for dir_name, info in (state.get("profile", {}).get("info_cache", {}) or {}).items():
            profiles.append({
                "dir": dir_name,
                "name": info.get("name") or dir_name,
                "email": info.get("user_name") or "",
            })
    except Exception:
        pass
    return profiles


def resolve_chrome_profile(hint):
    """Match a profile by folder name, display name or account email."""
    profiles = list_chrome_profiles()
    if not profiles:
        return None
    if not hint:
        return profiles[0]

    needle = hint.strip().lower()
    for profile in profiles:
        if needle in (profile["dir"].lower(), profile["name"].lower(), profile["email"].lower()):
            return profile
    for profile in profiles:
        if (needle in profile["dir"].lower() or needle in profile["name"].lower()
                or needle in profile["email"].lower()):
            return profile
    return None


def parse_session_cookies(raw):
    """Parse whatever was pasted into a list of (name, value) cookies.

    claude.ai uses several session cookies side by side (sessionKey,
    sessionKeyV3 and their *LC variants), so rather than guessing which one is
    authoritative this accepts any of these shapes:
      - a bare value                      -> treated as sessionKey
      - "sessionKey=abc; sessionKeyV3=de" -> a whole Cookie header
      - one "name=value" (or "name<TAB>value") per line
    """
    if not raw:
        return []

    text = str(raw).replace("\ufeff", "").replace("\x00", "")
    chunks = [c.strip() for c in re.split(r"[\r\n;]+", text)]
    chunks = [c for c in chunks if c and not c.startswith("#")]

    found = []
    for chunk in chunks:
        match = re.match(r'^["\']?([A-Za-z0-9_.\-]+)["\']?\s*[=\t:]\s*["\']?(.+?)["\']?$', chunk)
        if match:
            found.append((match.group(1), match.group(2).strip()))
        elif not re.search(r"\s", chunk):
            found.append(("sessionKey", chunk.strip("\"'")))

    # Keep only session cookies; the last occurrence of a name wins.
    by_name = {}
    for name, value in found:
        if name.lower().startswith("sessionkey") and value:
            by_name[name] = value
    return list(by_name.items())


def load_dot_env(file_path):
    """Minimal .env reader (no dependency): KEY=value lines, # comments, quotes."""
    env = {}
    if not os.path.exists(file_path):
        return env
    with open(file_path, "r", encoding="utf-8") as fh:
        for line in fh:
            line = line.strip().lstrip("\ufeff")
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, value = line.split("=", 1)
            key = key.strip()
            if key.startswith("export "):
                key = key[len("export "):].strip()
            value = value.strip().strip("\"'")
            if key and value:
                env[key] = value
    return env


def cookie_name_from_env_key(key):
    """Map a .env key to the claude.ai cookie it holds.

      SESSION_KEY -> sessionKey, SESSION_KEYV3 -> sessionKeyV3,
      SESSION_KEYV3LC -> sessionKeyV3LC

    A trailing _v2 / _2 marks a second value for the same cookie name (claude.ai
    lists some of these twice, once host-only and once domain-wide).
    """
    match = re.match(r"^SESSION_KEY([A-Za-z0-9]*?)(?:_v?\d+)?$", key, re.IGNORECASE)
    if not match:
        return None
    return "sessionKey" + (match.group(1) or "").upper()


def cookies_from_env(env):
    cookies = []
    for key, value in env.items():
        name = cookie_name_from_env_key(key)
        if name and value:
            cookies.append((name, value))
    return cookies


def read_session_cookies(explicit=None):
    """Session cookies let the bot reuse an existing Claude session instead of
    signing in again (which would add another device to the session list).

    Precedence: --session-key, CLAUDE_SESSION_KEY, session-key.txt, .env.
    """
    if explicit:
        return parse_session_cookies(explicit)
    if os.environ.get("CLAUDE_SESSION_KEY"):
        return parse_session_cookies(os.environ["CLAUDE_SESSION_KEY"])

    here = os.path.dirname(os.path.abspath(__file__))
    key_file = os.path.join(here, "session-key.txt")
    if os.path.exists(key_file):
        with open(key_file, "r", encoding="utf-8") as fh:
            from_file = parse_session_cookies(fh.read())
            if from_file:
                return from_file

    from_dot_env = cookies_from_env(load_dot_env(os.path.join(here, ".env")))
    if from_dot_env:
        return from_dot_env

    return cookies_from_env(os.environ)


def read_session_key(explicit=None):
    """Backwards-compatible single-key accessor."""
    cookies = read_session_cookies(explicit)
    return cookies[0][1] if cookies else None


async def is_signed_in(page):
    """Strict signed-in probe: not on the login flow AND account content present."""
    url = page.url
    if "/login" in url or "/magic-link" in url or "/onboarding" in url:
        return False
    try:
        if "Just a moment" in await page.title():
            return False
    except Exception:
        return False

    for marker in ("text=Active sessions", "text=Log out of all devices",
                   '[data-testid="user-menu-button"]'):
        try:
            if await page.locator(marker).first.is_visible():
                return True
        except Exception:
            continue
    return False


def is_port_open(port: int = 9222) -> bool:
    try:
        req = urllib.request.Request(f"http://127.0.0.1:{port}/json/version")
        with urllib.request.urlopen(req, timeout=1) as resp:
            return resp.status == 200
    except Exception:
        return False


def get_timestamp() -> str:
    return datetime.datetime.now().strftime("%H:%M:%S")


# Any logout control is off-limits to the bot. The settings page carries a plain
# "Log out" button (and, on some accounts, "Log out of all devices"); clicking
# either would end sessions this bot is meant to protect. Terminating a single
# session goes through the row menu, which never says "log out".
FORBIDDEN_CLICK_TEXT = re.compile(r"log\s*out|sign\s*out", re.I)

LOGOUT_GUARD_JS = r"""
() => {
  if (window.__claudeBotLogoutGuard) return;
  window.__claudeBotLogoutGuard = true;
  const FORBIDDEN = /log\s*out|sign\s*out/i;
  const block = event => {
    const el = event.target && event.target.closest
      ? event.target.closest('button, a, [role="menuitem"], [role="button"]')
      : null;
    if (!el) return;
    const label = (el.innerText || el.textContent || '') + ' ' + (el.getAttribute('aria-label') || '');
    if (FORBIDDEN.test(label)) {
      event.preventDefault();
      event.stopImmediatePropagation();
      console.warn('[claude-bot] Blocked a click on:', label.trim());
    }
  };
  for (const type of ['pointerdown', 'mousedown', 'click']) {
    document.addEventListener(type, block, true);
  }
}
"""


async def install_logout_guard(page):
    """Swallow any click on a "Log out of all devices" control before it reaches
    the app. This is the hard stop: even a stray click cannot trigger it.
    Applies to this automated browser window only."""
    try:
        await page.add_init_script(LOGOUT_GUARD_JS)
    except Exception:
        pass
    try:
        await page.evaluate(LOGOUT_GUARD_JS)
    except Exception:
        pass


async def safe_click(locator, what="element"):
    """Click only after checking this is not a "log out of all devices" control."""
    try:
        label = (await locator.inner_text()).strip()
    except Exception:
        label = ""
    try:
        aria = await locator.get_attribute("aria-label") or ""
    except Exception:
        aria = ""
    if FORBIDDEN_CLICK_TEXT.search(f"{label} {aria}"):
        raise RuntimeError(
            f'Refused to click {what}: its label ("{label}") would log out all devices.')
    await locator.click()


async def confirm_termination(page, timeout_seconds=8.0):
    """Confirm Claude's "Terminate session" dialog.

    Deliberately strict: a button is clicked only when its own label is exactly
    "Terminate". The settings page also carries a "Log out of all devices"
    button, and a loose text match there would sign out every session, protected
    ones included - so no such fallback exists here.
    """
    # Target the confirmation modal specifically: it is an alertdialog headed
    # "Terminate session". The settings panel itself is also role="dialog", so
    # an unfiltered lookup would land on the wrong element.
    dialog = await first_visible(page, [
        page.locator('[role="alertdialog"]').filter(
            has_text=re.compile("terminate session", re.I)),
        page.get_by_role("alertdialog"),
        page.locator('[role="dialog"]').filter(
            has_text=re.compile("terminate session", re.I)),
    ], timeout_seconds)

    if dialog is None:
        return False

    button = await first_visible(page, [
        dialog.get_by_role("button", name=re.compile(r"^\s*terminate\s*$", re.I)),
        dialog.locator("button").filter(has_text=re.compile(r"^\s*terminate\s*$", re.I)),
    ], 4.0)

    if button is None:
        return False

    label = (await button.inner_text()).strip()
    if not re.fullmatch(r"terminate", label, re.I):
        print(f'  --> Refusing to click confirm button labelled "{label}".')
        return False

    await safe_click(button, "the confirm button")
    return True


async def read_all_rows(page):
    """Read every session row's text in one pass (device, location, created)."""
    rows = await page.locator("table tr").all()
    out = []
    for i in range(1, len(rows)):
        cells = await rows[i].locator("th, td").all()
        if len(cells) < 2:
            continue
        out.append({
            "deviceText": (await cells[0].inner_text()).strip(),
            "locationText": (await cells[1].inner_text()).strip(),
            "createdText": (await cells[2].inner_text()).strip() if len(cells) >= 3 else "",
        })
    return out


async def first_visible(page, candidates, timeout_seconds=5.0):
    """Return the first locator that becomes visible, else None.

    Used instead of one comma-joined selector, which Playwright rejects when it
    mixes CSS with a `text=` engine.
    """
    deadline = asyncio.get_event_loop().time() + timeout_seconds
    while asyncio.get_event_loop().time() < deadline:
        for candidate in candidates:
            first = candidate.first
            try:
                if await first.is_visible():
                    return first
            except Exception:
                continue
        await asyncio.sleep(0.2)
    return None


async def reread_row(row):
    """Re-read a row's cells straight from the live DOM (used right before acting)."""
    cells = await row.locator("th, td").all()
    if len(cells) < 2:
        return None
    dev_text = (await cells[0].inner_text()).strip()
    loc_text = (await cells[1].inner_text()).strip()
    is_current = ("current" in dev_text.lower() or
                  await row.locator("text=Current").count() > 0 or
                  await row.locator('button[aria-label*="current session"]').count() > 0)
    return dev_text, loc_text, is_current


async def perform_scan(page, allowed_locations: List[str], dry_run: bool, check_num: int):
    settings_url = SETTINGS_URL
    time_str = get_timestamp()
    print(f"\n[{time_str}] Check #{check_num}: Inspecting active sessions...")

    try:
        curr_url = page.url
        if "/settings/account" not in curr_url:
            await page.goto(settings_url, wait_until="domcontentloaded", timeout=30000)
        else:
            await page.reload(wait_until="domcontentloaded", timeout=30000)
    except Exception as e:
        print(f"[{time_str}] Navigation warning: {e}")

    # Re-arm the guard after every navigation/reload.
    await install_logout_guard(page)

    if not await page.locator("text=Active sessions").is_visible():
        await page.goto(settings_url, wait_until="domcontentloaded", timeout=30000)
        await asyncio.sleep(2)

    try:
        await page.wait_for_selector("text=Active sessions", timeout=15000)
    except Exception:
        print(f"[{time_str}] Could not find 'Active sessions'. Will retry next interval.")
        return

    try:
        await page.evaluate("""() => {
            document.querySelectorAll('div[class*="overflow-y-auto"]').forEach(s => { s.scrollTop = s.scrollHeight; });
        }""")
        await asyncio.sleep(1.0)
    except Exception:
        pass

    terminated_count = 0
    kept_count = 0
    processed_keys: Set[str] = set()

    continue_scanning = True
    while continue_scanning:
        continue_scanning = False
        rows = await page.locator("table tr").all()

        if len(rows) <= 1:
            break

        row_to_terminate = None
        session_info = None
        readable_rows = 0
        keepable_rows = 0

        for i in range(1, len(rows)):
            row = rows[i]
            cells = await row.locator("th, td").all()
            if len(cells) < 2:
                continue

            dev_text = (await cells[0].inner_text()).strip()
            loc_text = (await cells[1].inner_text()).strip()
            created_text = (await cells[2].inner_text()).strip() if len(cells) >= 3 else ""

            is_current = ("current" in dev_text.lower() or
                          await row.locator("text=Current").count() > 0 or
                          await row.locator('button[aria-label*="current session"]').count() > 0)

            s_key = f"{dev_text}_{loc_text}_{created_text}"
            keep, reason = classify_session(dev_text, loc_text, is_current, allowed_locations)

            readable_rows += 1
            if keep:
                keepable_rows += 1
                if s_key not in processed_keys:
                    print(f"  [KEEP]  {dev_text.replace(chr(10), ' ')} | Location: {loc_text or 'N/A'} | {reason}")
                    processed_keys.add(s_key)
                    kept_count += 1
                continue

            # Remember it, but only act once the whole table has been read so the
            # sanity check below can veto it.
            if row_to_terminate is None:
                row_to_terminate = row
                session_info = {
                    "device": dev_text,
                    "location": loc_text,
                    "created": created_text,
                    "key": s_key,
                }

        # Runaway guard: a healthy account always keeps at least the current
        # session. If every row looks unprotected the page was misread.
        if row_to_terminate is not None and readable_rows > 1 and keepable_rows == 0:
            print("  [ABORT] Every row looked unprotected, which is almost certainly a "
                  "misread page. Nothing terminated.")
            break

        if row_to_terminate and session_info:
            dev = session_info["device"]
            loc = session_info["location"]
            created = session_info["created"]
            s_key = session_info["key"]

            if dry_run:
                print(f"  [WOULD TERMINATE] {dev} | Location: {loc or 'Unknown'} | Created: {created}")
                processed_keys.add(s_key)
                terminated_count += 1
                continue_scanning = False
            else:
                # Last-moment re-read: the table may have re-rendered since it was
                # scanned. Only act if the row still reads the same and still
                # classifies as unprotected.
                recheck = await reread_row(row_to_terminate)
                if recheck is None:
                    print("  [SKIP] Row vanished before termination - re-scanning next round.")
                    continue_scanning = True
                    continue
                re_dev, re_loc, re_current = recheck
                re_keep, re_reason = classify_session(re_dev, re_loc, re_current, allowed_locations)
                if re_keep or re_loc != loc or re_dev != dev:
                    print(f'  [SKIP] Row changed on re-check (now "{re_dev}" @ "{re_loc}" - '
                          f'{re_reason}). Not terminating.')
                    processed_keys.add(s_key)
                    kept_count += 1
                    continue_scanning = True
                    continue

                print(f"  [TERMINATING] {dev} | Location: {loc or 'Unknown'} | Created: {created}...")
                try:
                    action_btn = row_to_terminate.locator(
                        'button[aria-haspopup="menu"], button[aria-label*="Session actions"], button'
                    ).first
                    await action_btn.scroll_into_view_if_needed()
                    await safe_click(action_btn, "the row action button")
                    await asyncio.sleep(0.5)

                    # Playwright rejects a selector list that mixes CSS with a
                    # `text=` engine, so each candidate is tried on its own.
                    terminate_item = await first_visible(page, [
                        page.get_by_role("menuitem", name=re.compile("terminate", re.I)),
                        page.locator('[role="menu"] button').filter(
                            has_text=re.compile("terminate", re.I)),
                        page.locator('[role="menuitem"]:has-text("Terminate")'),
                        page.locator('[data-part="item"]:has-text("Terminate")'),
                        page.get_by_text("Terminate", exact=False),
                    ])
                    if terminate_item is None:
                        raise RuntimeError('No "Terminate" menu item appeared.')
                    await safe_click(terminate_item, "the Terminate menu item")
                    await asyncio.sleep(0.5)

                    # Claude asks "Terminate session - are you sure?" first.
                    confirmed = await confirm_termination(page)
                    if not confirmed:
                        print("  --> Could not confirm in the dialog; nothing was terminated.")
                        await page.keyboard.press("Escape")
                        await asyncio.sleep(0.3)
                        continue_scanning = True
                        continue

                    # Only count it once the row is actually gone from the table.
                    await asyncio.sleep(1.5)
                    still_there = any(
                        r["deviceText"] == dev and r["locationText"] == loc
                        and r["createdText"] == created
                        for r in await read_all_rows(page)
                    )
                    if still_there:
                        print("  --> Session still listed after confirming; will retry next round.")
                    else:
                        print("  --> Successfully terminated session!")
                        terminated_count += 1

                    continue_scanning = True
                except Exception as term_err:
                    print(f"  --> Error terminating session: {term_err}")

    if dry_run:
        rows = await page.locator("table tr").all()
        for i in range(1, len(rows)):
            row = rows[i]
            cells = await row.locator("th, td").all()
            if len(cells) < 2:
                continue
            dev = (await cells[0].inner_text()).strip()
            loc = (await cells[1].inner_text()).strip()
            created = (await cells[2].inner_text()).strip() if len(cells) >= 3 else ""
            s_key = f"{dev}_{loc}_{created}"

            if s_key in processed_keys:
                continue

            is_curr = "current" in dev.lower() or await row.locator("text=Current").count() > 0
            keep, reason = classify_session(dev, loc, is_curr, allowed_locations)
            if keep:
                print(f"  [KEEP]  {dev.replace(chr(10), ' ')} | Location: {loc or 'N/A'} | {reason}")
                kept_count += 1
            else:
                print(f"  [WOULD TERMINATE] {dev} | Location: {loc} | Created: {created}")
                terminated_count += 1
            processed_keys.add(s_key)

    total = len(processed_keys)
    action_label = "Flagged" if dry_run else "Terminated"
    print(f"[{time_str}] Check complete. Total: {total} | Kept: {kept_count} | {action_label}: {terminated_count}")


async def main():
    parser = argparse.ArgumentParser(description="Claude Active Sessions Terminator Bot (Playwright)")
    parser.add_argument("--interval", "-i", type=int, default=60, help="Check interval in seconds (default: 60 = 1 minute)")
    parser.add_argument("--once", action="store_true", help="Run a single scan and exit")
    parser.add_argument("--dry-run", action="store_true", help="Preview sessions without terminating")
    parser.add_argument("--allow", "-a", type=str, default="",
                        help="EXTRA protected locations, comma-separated. Added to the "
                             f"always-protected list ({', '.join(PROTECTED_LOCATIONS)}), which cannot be disabled.")
    parser.add_argument("--chrome-path", type=str, default=None,
                        help="Path to chrome.exe (default: auto-detected local install)")
    parser.add_argument("--chrome-profile", type=str, default=DEFAULT_CHROME_PROFILE,
                        help=f"Local Chrome profile: folder, display name or email (default: {DEFAULT_CHROME_PROFILE})")
    parser.add_argument("--session-key", type=str, default=None,
                        help="Reuse existing claude.ai session cookies instead of signing in. "
                             "Accepts a bare sessionKey value or a whole cookie string "
                             "(also read from session-key.txt or CLAUDE_SESSION_KEY)")
    parser.add_argument("--cdp", type=str, default=None, help="Connect via CDP to running browser port (e.g. 9222)")
    parser.add_argument("--user-data-dir", type=str, default="./chrome_session",
                        help="Persistent browser profile directory (default: ./chrome_session)")
    parser.add_argument("--headless", action="store_true", help="Run browser in headless mode")

    args = parser.parse_args()
    extra_locations = [loc.strip().lower() for loc in args.allow.split(",") if loc.strip()]
    allowed_locations = list(dict.fromkeys(PROTECTED_LOCATIONS + extra_locations))
    user_data_dir = os.path.abspath(args.user_data_dir)

    print("=" * 65)
    print("  Claude Active Sessions Manager - Python Bot")
    print("=" * 65)
    print(f"Protected (always): {', '.join(PROTECTED_LOCATIONS)}")
    if extra_locations:
        print(f"Protected (extra) : {', '.join(extra_locations)}")
    print(f"Mode              : {'DRY-RUN (Preview only)' if args.dry_run else 'LIVE (Auto-terminate unprotected)'}")
    print(f"Interval          : {'Single scan (--once)' if args.once else f'Every {args.interval}s (Every-minute bot)'}")
    print(f"Session Dir       : {user_data_dir}")

    print("=" * 65)

    try:
        from playwright.async_api import async_playwright
    except ImportError:
        print("\nError: Playwright is not installed for Python.")
        print("Install it using: pip install playwright && playwright install chromium\n")
        return

    async with async_playwright() as p:
        browser = None
        context = None
        page = None
        is_cdp = False

        cdp_port = args.cdp or ("9222" if is_port_open(9222) else None)

        if cdp_port:
            cdp_url = f"http://127.0.0.1:{cdp_port}"
            print(f"\nConnecting to browser over CDP ({cdp_url})...")
            browser = await p.chromium.connect_over_cdp(cdp_url)
            contexts = browser.contexts
            context = contexts[0] if contexts else await browser.new_context()

            for existing_page in context.pages:
                if "claude.ai" in existing_page.url:
                    page = existing_page
                    print(f"Found existing Claude tab: {page.url}")
                    await page.bring_to_front()
                    break

            if not page:
                page = await context.new_page()
            is_cdp = True
        else:
            os.makedirs(user_data_dir, exist_ok=True)

            chrome_path = find_chrome_executable(args.chrome_path)
            if chrome_path:
                print("")
                print(f"Using your local Chrome: {chrome_path}")
            else:
                print("")
                print("Could not find a local Chrome install; falling back to Playwright Chromium.")

            profile = resolve_chrome_profile(args.chrome_profile)
            if profile:
                label = profile.get("email") or profile.get("name")
                print(f"Chrome account    : {label} ({profile['dir']})")
            elif args.chrome_profile:
                print(f"Chrome account    : no local profile matched \"{args.chrome_profile}\"")

            launch_kwargs = {
                "headless": args.headless,
                "viewport": {"width": 1280, "height": 850},
                "args": [
                    "--disable-blink-features=AutomationControlled",
                    "--no-default-browser-check",
                    "--no-first-run",
                ],
            }

            if chrome_path:
                context = await p.chromium.launch_persistent_context(
                    user_data_dir,
                    executable_path=chrome_path,
                    **launch_kwargs,
                )
            else:
                try:
                    context = await p.chromium.launch_persistent_context(
                        user_data_dir,
                        channel="chrome",
                        **launch_kwargs,
                    )
                except Exception:
                    context = await p.chromium.launch_persistent_context(
                        user_data_dir,
                        **launch_kwargs,
                    )

            session_cookies = read_session_cookies(args.session_key)
            if session_cookies:
                names = ", ".join(name for name, _ in session_cookies)
                print(f"Reusing your existing claude.ai session ({names}) - no new sign-in.")
                # Set each cookie on both the host and the parent domain: claude.ai
                # issues some host-only and some domain-wide, and a mismatch reads
                # as signed out.
                by_name = {}
                for name, value in session_cookies:
                    by_name.setdefault(name, []).append(value)

                payload = []
                for name, values in by_name.items():
                    for i, value in enumerate(values[:2]):
                        # One value: write it to both domains so the shape cannot
                        # be wrong. Two values: first domain-wide, second host-only.
                        domains = [(".claude.ai", "claude.ai")[i]] if len(values) > 1 \
                            else [".claude.ai", "claude.ai"]
                        for domain in domains:
                            payload.append({
                                "name": name,
                                "value": value,
                                "domain": domain,
                                "path": "/",
                                "httpOnly": True,
                                "secure": True,
                                "sameSite": "Lax",
                            })
                await context.add_cookies(payload)

            page = context.pages[0] if context.pages else await context.new_page()

        settings_url = SETTINGS_URL
        print(f"\nNavigating to {settings_url}...")
        await page.goto(settings_url, wait_until="domcontentloaded", timeout=45000)

        print("Checking authentication status...")
        max_wait_seconds = 300
        elapsed = 0
        logged_in = False
        prompted = False

        while elapsed < max_wait_seconds:
            if await is_signed_in(page):
                logged_in = True
                break

            curr_url = page.url
            try:
                title = await page.title()
            except Exception:
                title = ""
            on_login = "/login" in curr_url or "/magic-link" in curr_url

            if (on_login or "Just a moment" in title) and not prompted:
                prompted = True
                print("")
                print("[Action required] This browser profile is not signed in to Claude.")
                print("Sign in once in the window that just opened - it is remembered from then on.")
                if not read_session_cookies(args.session_key):
                    print('To avoid a sign-in entirely, copy your claude.ai session cookies out')
                    print("of your everyday Chrome (F12 > Application > Cookies > claude.ai) and")
                    print("save them with:  node save-session-key.js")
                print("")

            if on_login:
                try:
                    await page.wait_for_url(lambda u: "/login" not in u, timeout=30000)
                    await page.goto(settings_url, wait_until="domcontentloaded", timeout=45000)
                except Exception:
                    pass

            await asyncio.sleep(2)
            elapsed += 2

        if not logged_in:
            print("Authentication timed out. Please log in and re-run.")
            return

        print("Authentication confirmed! Bot is active.")

        await install_logout_guard(page)
        print('Safety guard armed: every "Log out" control is blocked in this window.')

        check_num = 1
        try:
            while True:
                await perform_scan(page, allowed_locations, args.dry_run, check_num)
                check_num += 1

                if args.once:
                    print("\nSingle scan completed (--once). Exiting.")
                    break

                print(f"\n⏳ Next check in {args.interval}s (Press Ctrl+C to stop)...")
                await asyncio.sleep(args.interval)
        except (KeyboardInterrupt, asyncio.CancelledError):
            print("\n\nStopping bot cleanly...")
        finally:
            if not is_cdp and context:
                await context.close()
            elif is_cdp and browser:
                await browser.close()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nExited.")
