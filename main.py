#!/usr/bin/env python3
"""
Claude Active Sessions Manager (Playwright - Python) - Every-Minute Bot

Continuously monitors active Claude sessions and terminates any session
not located in allowed locations (default: Mumbai, Navi Mumbai, Bergen).
Never terminates the current active session.
"""

import argparse
import asyncio
import datetime
import os
import signal
import sys
import urllib.request
from typing import List, Set

DEFAULT_ALLOWED_LOCATIONS = ["mumbai", "navi mumbai", "bergen", "panvel"]


def is_location_allowed(location: str, allowed_locations: List[str]) -> bool:
    if not location:
        return False
    loc_lower = location.lower()
    return any(allowed.lower() in loc_lower for allowed in allowed_locations)


def is_port_open(port: int = 9222) -> bool:
    try:
        req = urllib.request.Request(f"http://127.0.0.1:{port}/json/version")
        with urllib.request.urlopen(req, timeout=1) as resp:
            return resp.status == 200
    except Exception:
        return False


def get_timestamp() -> str:
    return datetime.datetime.now().strftime("%H:%M:%S")


async def perform_scan(page, allowed_locations: List[str], dry_run: bool, check_num: int):
    settings_url = "https://claude.ai/new#settings/account"
    time_str = get_timestamp()
    print(f"\n[{time_str}] Check #{check_num}: Inspecting active sessions...")

    try:
        curr_url = page.url
        if "settings/account" not in curr_url:
            await page.goto(settings_url, wait_until="domcontentloaded", timeout=30000)
        else:
            await page.reload(wait_until="domcontentloaded", timeout=30000)
    except Exception as e:
        print(f"[{time_str}] Navigation warning: {e}")

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

            if is_current:
                if s_key not in processed_keys:
                    print(f"  [KEEP]  {dev_text.replace(chr(10), ' ')} | Location: {loc_text} (Current Session)")
                    processed_keys.add(s_key)
                    kept_count += 1
                continue

            allowed = is_location_allowed(loc_text, allowed_locations)
            if allowed:
                if s_key not in processed_keys:
                    print(f"  [KEEP]  {dev_text} | Location: {loc_text} | Created: {created_text}")
                    processed_keys.add(s_key)
                    kept_count += 1
                continue

            row_to_terminate = row
            session_info = {
                "device": dev_text,
                "location": loc_text,
                "created": created_text,
                "key": s_key,
            }
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
                print(f"  [TERMINATING] {dev} | Location: {loc or 'Unknown'} | Created: {created}...")
                try:
                    action_btn = row_to_terminate.locator(
                        'button[aria-haspopup="menu"], button[aria-label*="Session actions"], button'
                    ).first
                    await action_btn.scroll_into_view_if_needed()
                    await action_btn.click()
                    await asyncio.sleep(0.5)

                    terminate_item = page.locator(
                        '[role="menuitem"]:has-text("Terminate"), [role="menu"] button:has-text("Terminate"), text=Terminate'
                    ).first
                    await terminate_item.wait_for(state="visible", timeout=5000)
                    await terminate_item.click()
                    await asyncio.sleep(0.5)

                    try:
                        confirm_btn = page.locator(
                            '[role="dialog"] button:has-text("Terminate"), [role="alertdialog"] button:has-text("Terminate"), button:has-text("Log out")'
                        ).first
                        if await confirm_btn.is_visible(timeout=2000):
                            await confirm_btn.click()
                    except Exception:
                        pass

                    print(f"  --> Successfully terminated session!")
                    terminated_count += 1
                    await asyncio.sleep(1.5)
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
            if is_curr or is_location_allowed(loc, allowed_locations):
                print(f"  [KEEP]  {dev.replace(chr(10), ' ')} | Location: {loc}")
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
    parser.add_argument("--allow", "-a", type=str, default="mumbai,navi mumbai,bergen,panvel",
                        help="Comma-separated list of allowed locations (default: 'mumbai,navi mumbai,bergen,panvel')")
    parser.add_argument("--cdp", type=str, default=None, help="Connect via CDP to running browser port (e.g. 9222)")
    parser.add_argument("--user-data-dir", type=str, default="./chrome_session",
                        help="Persistent browser profile directory (default: ./chrome_session)")
    parser.add_argument("--headless", action="store_true", help="Run browser in headless mode")

    args = parser.parse_args()
    allowed_locations = [loc.strip().lower() for loc in args.allow.split(",") if loc.strip()]
    user_data_dir = os.path.abspath(args.user_data_dir)

    print("=" * 65)
    print("  Claude Active Sessions Manager - Python Bot")
    print("=" * 65)
    print(f"Allowed Locations : {', '.join(allowed_locations)}")
    print(f"Mode              : {'DRY-RUN (Preview only)' if args.dry_run else 'LIVE (Auto-terminate unauthorized)'}")
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
            print(f"\nLaunching desktop browser with persistent profile...")

            launch_kwargs = {
                "headless": args.headless,
                "viewport": {"width": 1280, "height": 850},
                "args": [
                    "--disable-blink-features=AutomationControlled",
                    "--no-default-browser-check",
                ],
            }

            try:
                context = await p.chromium.launch_persistent_context(
                    user_data_dir,
                    channel="chrome",
                    **launch_kwargs,
                )
            except Exception:
                print("Chrome channel not found, launching default Chromium...")
                context = await p.chromium.launch_persistent_context(
                    user_data_dir,
                    **launch_kwargs,
                )

            page = context.pages[0] if context.pages else await context.new_page()

        settings_url = "https://claude.ai/new#settings/account"
        print(f"\nNavigating to {settings_url}...")
        await page.goto(settings_url, wait_until="domcontentloaded", timeout=45000)

        print("Checking authentication status...")
        max_wait_seconds = 120
        elapsed = 0
        logged_in = False

        while elapsed < max_wait_seconds:
            curr_url = page.url
            title = await page.title()

            if "/login" in curr_url or "Just a moment" in title:
                print("\n[Action Required] Claude is prompting for login or verification.")
                print("Please log in to your Claude account in the opened browser window.")
                print("Waiting for login to complete (session is saved for future runs)...\n")
                try:
                    await page.wait_for_url(
                        lambda u: "/login" not in u and "turnstile" not in u,
                        timeout=30000,
                    )
                except Exception:
                    pass

            has_active_sessions = await page.locator("text=Active sessions").is_visible()
            has_account = await page.locator("text=Log out of all devices").is_visible()
            has_settings = await page.locator('[aria-label="Settings"], button:has-text("Settings")').is_visible()
            has_user_menu = await page.locator('[data-testid="user-menu-button"]').is_visible()

            if has_active_sessions or has_account or has_settings or has_user_menu:
                logged_in = True
                break

            await asyncio.sleep(2)
            elapsed += 2

        if not logged_in:
            print("Authentication timed out. Please log in and re-run.")
            return

        print("Authentication confirmed! Bot is active.")

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
