# Claude Chokidar

A watchdog for your Claude account's **Active Sessions**. It checks the session
list on a schedule and terminates any session that is not from a location you
have protected — while making it very hard to ever terminate one you care about.

It runs on your **locally installed Chrome** and signs in by reusing your
**existing claude.ai session cookie**, so it does not create a second login or
add another device to your session list.

---

## Contents

- [Protected locations](#protected-locations)
- [Safety rules](#safety-rules)
- [Requirements](#requirements)
- [Setup](#setup)
  - [1. Install](#1-install)
  - [2. Get your session cookie](#2-get-your-session-cookie)
  - [3. Create your `.env`](#3-create-your-env)
  - [4. Check it works](#4-check-it-works)
- [Running the bot](#running-the-bot)
  - [Scan intervals](#scan-intervals)
  - [Running in the background](#running-in-the-background)
- [All command-line options](#all-command-line-options)
- [How sign-in works](#how-sign-in-works)
- [Troubleshooting](#troubleshooting)
- [Python version](#python-version)
- [Files in this repo](#files-in-this-repo)

---

## Protected locations

These are **always protected** and can never be terminated. They are hard-coded
in `index.js` (`PROTECTED_LOCATIONS`); the `--allow` flag can only *add* more
locations, never remove one:

| | Location | Matches, for example |
|---|---|---|
| ✅ | Mumbai | `Mumbai, Maharashtra, IN` |
| ✅ | Navi Mumbai | `Navi Mumbai, Maharashtra, IN` |
| ✅ | Panvel | `Panvel, Maharashtra, IN` |
| ✅ | Bergen | `Bergen, Vestland, NO` |
| ✅ | The current session | the browser the bot itself is using |
| ❌ | anything else | terminated |

Matching is case-insensitive and substring-based, so a city matches regardless
of the state/country suffix Claude appends.

To protect an extra city without touching the code:

```bash
node index.js --allow "oslo,pune"
```

---

## Safety rules

Beyond the location list, four rules make a wrong termination hard:

1. **Unknown location = keep.** If the location cell is blank or unreadable
   (slow render, layout change), the row is kept, never terminated.
2. **Re-check before acting.** The row is re-read from the live DOM immediately
   before the terminate click. If it now reads differently, or now classifies as
   protected, the bot skips it and re-scans.
3. **Runaway guard.** If *every* row looks unprotected — impossible on a healthy
   account, since the current session is always protected — the bot assumes it
   misread the page and terminates nothing that round.
4. **"Log out" can never be clicked.** The account page carries a plain
   `Log out` button (and on some accounts `Log out of all devices`), which would
   end sessions this bot exists to protect. Two independent layers stop it: a
   capture-phase listener injected into the page swallows any click on a logout
   control before the app sees it, and every click the bot makes is checked
   against the element's own label first. Terminating a single session goes
   through the row menu and its `Terminate session` confirmation, neither of
   which says "log out".

A termination is only counted once the row has actually disappeared from the
table.

---

## Requirements

- **Node.js 18+**
- **Google Chrome** installed locally (the bot drives `chrome.exe`; it does not
  need Playwright's bundled Chromium)
- A Claude account you are signed in to in that Chrome

---

## Setup

### 1. Install

```bash
git clone git@github.com:Amrit-Nigam/claude-chokidar.git
cd claude-chokidar
npm install
```

### 2. Get your session cookie

The bot authenticates as *you* by reusing your existing claude.ai session
cookie. To copy it:

1. Open <https://claude.ai> in the Chrome where you are already signed in.
2. Press **F12** to open DevTools.
3. Go to the **Application** tab.
4. In the left sidebar: **Storage → Cookies → `https://claude.ai`**.
5. Find the row named exactly **`sessionKey`**.
6. Click it and copy the **Value** — it starts with `sk-ant-sid02-` and is
   around 130 characters long.

**Which cookie exactly?** You will see several similar names. Use this mapping:

| Cookie name in Chrome | `.env` key | Needed? |
|---|---|---|
| `sessionKey` | `SESSION_KEY` | **Yes — this is the one** |
| `sessionKeyV3` | `SESSION_KEYV3` | Optional |
| `sessionKeyLC` | `SESSION_KEYLC` | Optional |
| `sessionKeyV3LC` | `SESSION_KEYV3LC` | Optional |

`SESSION_KEY` on its own is enough — that is the configuration this project is
tested with. Add the optional ones only if sign-in ever fails without them.

If the cookie list shows the **same name twice** (claude.ai keeps one host-only
and one domain-wide copy), put the second value in a `_v2` key, for example
`SESSION_KEYV3_v2`.

Copy the **Value** only — not the cookie name, not quotes, not a trailing
semicolon.

### 3. Create your `.env`

```bash
cp .env.example .env
```

Then edit `.env`:

```ini
SESSION_KEY=sk-ant-sid02-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx
```

`.env` is gitignored. Treat it like a password — anyone holding that value can
use your Claude account.

> **On Windows, do not create it with `echo ... > .env` in PowerShell.**
> PowerShell writes UTF-16, which corrupts the value. Use an editor, or run:
> ```bash
> npm run session-key -- "sk-ant-sid02-..."
> ```
> which writes a correctly encoded `session-key.txt` instead.

### 4. Check it works

Run one scan in preview mode. Nothing is ever terminated in `--dry-run`:

```bash
npm run dry-run
```

Expected output:

```
Protected (always): mumbai, navi mumbai, bergen, panvel
Mode              : DRY-RUN (Preview only)

Using your local Chrome: C:\Program Files\Google\Chrome\Application\chrome.exe
Reusing your existing claude.ai session (sessionKey) - no new sign-in.
Safety guard armed: every "Log out" control is blocked in this window.

[17:13:55] Check #1: Inspecting active sessions...
  [KEEP]  Chrome (Mac OS X)        | Location: Mumbai, Maharashtra, IN | Protected location
  [KEEP]  Chrome (Windows) Current | Location: Mumbai, Maharashtra, IN | Current session
  [KEEP]  Claude (iOS)             | Location: Bergen, Vestland, NO    | Protected location
  [WOULD TERMINATE] Chrome (Linux) | Location: Delhi, IN | Created: ...
[17:13:55] Check complete. Total: 4 | Kept: 3 | Flagged: 1
```

Check that the `[KEEP]` lines list everything you expect to survive, then run it
for real.

---

## Running the bot

```bash
npm start
```

That starts the watchdog and scans every 60 seconds until you stop it with
`Ctrl + C`.

Other one-off modes:

```bash
npm run once         # scan once, terminate, exit
npm run dry-run      # scan once, terminate nothing, exit
npm run bot:dry-run  # keep scanning, terminate nothing (monitoring mode)
```

### Scan intervals

Pick a ready-made interval:

| Interval | Command | Equivalent |
|---|---|---|
| Every 30 seconds | `npm run bot:30s` | `node index.js --interval 30` |
| **Every 1 minute** (default) | `npm run bot:1m` or `npm start` | `node index.js --interval 60` |
| Every 5 minutes | `npm run bot:5m` | `node index.js --interval 300` |
| Every 10 minutes | `npm run bot:10m` | `node index.js --interval 600` |
| Every 15 minutes | `npm run bot:15m` | `node index.js --interval 900` |

Or set any interval you like, in **seconds**:

```bash
node index.js --interval 120     # every 2 minutes
node index.js --interval 1800    # every 30 minutes
node index.js --interval 3600    # every hour
```

Between scans the bot prints when the next check is due:

```
⏳ Next check in 300s (Press Ctrl+C to stop)...
```

A shorter interval reacts faster; a longer one is gentler on the account and
your machine. Every 5 minutes is a good middle ground if 1 minute feels noisy.

### Running in the background

Once your `.env` works, add `--headless` so no window is shown.

**Windows (PowerShell)**

```powershell
# Start detached, logging to bot.log
Start-Process -WindowStyle Hidden -FilePath node `
  -ArgumentList "index.js","--interval","300","--headless" `
  -RedirectStandardOutput bot.log -RedirectStandardError bot.err

# Watch the log
Get-Content bot.log -Wait
```

To start it automatically at login, create a Task Scheduler task that runs
`node D:\path\to\claude-chokidar\index.js --interval 300 --headless`.

**macOS / Linux**

```bash
nohup node index.js --interval 300 --headless > bot.log 2>&1 &
tail -f bot.log
pkill -f "node index.js"
```

**With PM2 (any OS)**

```bash
npm install -g pm2
pm2 start index.js --name claude-chokidar -- --interval 300 --headless
pm2 logs claude-chokidar
pm2 save && pm2 startup     # survive reboots
```

---

## All command-line options

```text
Usage: node index.js [options]

  --interval, -i <sec>    Seconds between scans (default: 60)
  --once                  Run a single scan and exit
  --dry-run               Preview only; never terminates anything
  --allow, -a <list>      Extra protected locations, comma-separated.
                          ADDED to the always-protected list; the built-in
                          protected locations cannot be disabled.
  --session-key <value>   Session cookie to use (overrides .env)
  --chrome-profile <name> Local Chrome profile: folder, display name or email
  --chrome-path <exe>     Path to chrome.exe (default: auto-detected)
  --separate-profile      Never touch the real Chrome profile
  --cdp [port]            Attach to a Chrome already running with a debug port
  --user-data-dir <dir>   Where to persist the bot's browser data
                          (default: ./chrome_session)
  --headless              Run without a visible window
  --list-profiles         List local Chrome profiles and exit
  -h, --help              Show help
```

Examples:

```bash
# Every 15 minutes, headless, also protecting Oslo
node index.js --interval 900 --headless --allow "oslo"

# Use a different Chrome profile
node index.js --chrome-profile you@example.com

# See which profiles exist
npm run profiles
```

---

## How sign-in works

The bot reads your session cookies in this order of precedence:

1. `--session-key "<value>"` on the command line
2. the `CLAUDE_SESSION_KEY` environment variable
3. `session-key.txt` (written by `npm run session-key -- "<value>"`)
4. **`.env`** ← the normal setup

If none is configured, the bot opens a window and waits for you to sign in once;
that login is then remembered in `./chrome_session`. Note that signing in this
way *does* add one extra session to your list.

**Why it cannot just read your everyday Chrome profile.** Chrome blocks this by
design, and both doors are shut:

- Chrome 136+ refuses remote debugging when running on the default
  user-data-dir, so Playwright cannot drive your normal Chrome as-is.
- Chrome's app-bound cookie encryption (`v20` cookies) means a *copied* profile
  decrypts to nothing — the copy is always signed out.

Reusing the cookie is the supported way through. If you would rather attach to a
real browser, close Chrome completely and run `npm run chrome:debug`, then
`npm run connect`.

---

## Troubleshooting

**"This browser profile is not signed in to Claude"**
Your `SESSION_KEY` is expired, truncated, or was written as UTF-16. Re-copy the
`sessionKey` value from DevTools and update `.env`. Session cookies do expire —
expect to refresh this occasionally.

**Sign-in fails even with a fresh `SESSION_KEY`**
Add the optional `SESSION_KEYV3` (and `SESSION_KEYV3_v2` if the name appears
twice) as described in [step 2](#2-get-your-session-cookie).

**"Could not find 'Active sessions' table"**
The page did not finish rendering, or you are not signed in. The bot skips that
round and retries on the next interval; nothing is terminated.

**"Chrome is already running, so it cannot be switched into debug mode"**
Informational. The bot uses its own profile directory (`./chrome_session`) and
carries on — your everyday Chrome is untouched and stays usable.

**Nothing gets terminated**
Check the `[KEEP]` reason printed for each row. `Location unknown - kept for
safety` means the location cell did not render; `Protected location` means it
matched your protected list.

**A session you want removed is in a protected city**
That is deliberate: protected locations cannot be terminated by this bot. Remove
it from claude.ai's settings page yourself.

---

## Python version

`main.py` mirrors `index.js` — same protected locations, same safety rules, same
`.env` handling.

```bash
pip install -r requirements.txt
playwright install chromium

python main.py --dry-run --once      # preview
python main.py --interval 300        # every 5 minutes
```

> The Node version is the one that is actively exercised; the Python port is
> kept in step but is not equally tested.

---

## Files in this repo

| File | Purpose |
|---|---|
| `index.js` | The bot: scanning, classification, termination, safety guards |
| `main.py` | Python port of the same bot |
| `chromeProfile.js` | Finds your local `chrome.exe` and Chrome profiles |
| `save-session-key.js` | Writes `session-key.txt` with correct encoding |
| `launch-chrome-debug.js` | Starts your own Chrome with a debug port |
| `.env.example` | Template for your `.env` |
| `chrome_session/` | The bot's browser profile (gitignored) |

`.env`, `session-key.txt` and `chrome_session/` are all gitignored and must stay
out of version control.
