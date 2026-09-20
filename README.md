# Claude Active Sessions Terminator Bot (Playwright)

An automated Playwright bot that runs continuously, checking your Claude account's **Active Sessions** every minute (or custom interval) and terminating any session that is **not** from a protected location. It runs on your locally installed Chrome and reuses your existing claude.ai session, so it never adds a device of its own.

### Protected Locations Policy

These are **always protected** and can never be terminated. They are hard-coded;
`--allow` can only *add* more locations, never remove one:

- ✅ **Mumbai** (incl. "Mumbai, Maharashtra, IN")
- ✅ **Navi Mumbai**
- ✅ **Panvel**
- ✅ **Bergen** (incl. "Bergen, Vestland, NO")
- ✅ **Current session** (never logs itself out)
- ❌ Everything else is terminated

Four further safety rules make a wrong termination very hard:

1. **Unknown location = keep.** If the location cell is blank or unreadable
   (slow render, layout change), the row is kept, never terminated.
2. **Re-check before acting.** The row is re-read from the live DOM immediately
   before the terminate click; if it now reads differently, or now classifies as
   protected, the bot skips it and re-scans.
3. **Runaway guard.** If *every* row looks unprotected - which on a healthy
   account is impossible, since the current session is always protected - the
   bot assumes it misread the page and terminates nothing that round.
4. **The bot never logs itself out.** Its own browser appears in the list as an
   ordinary device, from wherever *this machine's* connection geolocates - which
   is usually not one of the protected cities. On first run the bot identifies
   that row (same platform, created when it signed in), pins it in
   `bot-session.json`, and never terminates it. Other sessions from that same
   city are still terminated normally. Re-pin with `--forget-self`.

---

## Which Browser It Uses

The bot drives your **locally installed Chrome** (`chrome.exe`), not Playwright's
bundled Chromium, and targets the `shreyans.tatiya@gmail.com` profile by default
(change with `--chrome-profile`). List what is available:

```bash
npm run profiles
```

### How it signs in (no second session is created)

The bot reuses your existing claude.ai session cookies, so it appears as your
*current* session rather than adding another device to the list.

Put them in a `.env` file in the project root (gitignored):

```ini
SESSION_KEY=sk-ant-sid02-...
SESSION_KEYV3=sk-ant-sid02-...
SESSION_KEYV3_v2=sk-ant-sid02-...
```

Key names map to cookie names: `SESSION_KEY` → `sessionKey`,
`SESSION_KEYV3` → `sessionKeyV3`, `SESSION_KEYLC` → `sessionKeyLC`, and so on.
A `_v2` suffix is a second value for the same cookie name (claude.ai lists some
of them twice - once host-only, once domain-wide).

To get the values: in your normal Chrome on claude.ai, press F12 →
Application → Cookies → `https://claude.ai` → copy each Value.

Alternatives, in order of precedence:

```bash
node index.js --session-key "sk-ant-sid02-..."   # 1. flag
set CLAUDE_SESSION_KEY=sk-ant-sid02-...          # 2. environment variable
node save-session-key.js "<paste cookie value>"  # 3. writes session-key.txt
                                                 # 4. .env (as above)
```

`save-session-key.js` also accepts a whole `cookie:` header pasted from
DevTools → Network, and writes the file as UTF-8 (PowerShell's `>` writes
UTF-16, which silently corrupts the value).

If no session is configured, the bot opens a window for a one-time sign-in
instead; that adds a single `Chrome (Windows)` session in Mumbai, which is a
protected location and so is never terminated.

**Why not just read your live Chrome profile?** Chrome blocks it, by design:
Chrome 136+ refuses remote debugging on the default user-data-dir, and
app-bound cookie encryption (v20 cookies) means a copied profile decrypts to
nothing. Cookie reuse is the supported way through.

---

## How the Bot Works

1. **Initial Run**: Starts your local Chrome and reuses your claude.ai session cookies (see "Which Browser It Uses" above), so no second session is created.
2. **Every Minute**: The bot opens `https://claude.ai/settings/account`, refreshes the active sessions table, and scans all connected devices.
3. **Automatic Termination**: Any session whose location is readable and not protected is terminated.
4. **Persistent & Resilient**: Handles network drops, page reload delays, or temporary disconnects without crashing. Clean shutdown on `Ctrl + C`.

---

## Quick Start

### 1. Run the Bot (Default: Checks every 60s)
```bash
npm start
# or
node index.js
```

### 2. Run in Preview / Monitor Mode (Dry Run)
Continually monitors active sessions every minute and alerts in the console without terminating anything:
```bash
npm run bot:dry-run
# or
node index.js --dry-run
```

### 3. Single Scan & Exit
If you only want to scan once and terminate unauthorized sessions immediately:
```bash
npm run once
# or
node index.js --once
```

---

## Command-Line Options

```text
Usage:
  node index.js [options]

Options:
  --interval, -i <sec>      Check interval in seconds (default: 60 = 1 minute)
  --once                    Run a single scan and exit (disables recurring loop)
  --dry-run                 Preview sessions without terminating
  --allow, -a <locations>   EXTRA protected locations (added to the always-protected
                            list; the protected list itself cannot be disabled)
  --chrome-profile <name>   Local Chrome profile: folder, display name or email
                            (default: shreyans.tatiya@gmail.com)
  --chrome-path <exe>       Path to chrome.exe (default: auto-detected)
  --session-key <value>     Reuse an existing claude.ai sessionKey cookie
  --separate-profile        Never touch the real Chrome profile
  --cdp [port]              Attach to a Chrome already running with a debug port
  --user-data-dir <dir>     Directory to persist browser login data (default: ./chrome_session)
  --headless                Run in headless mode (after the first sign-in)
  --forget-self             Forget which session is the bot's own and re-pin it
  --list-profiles           List local Chrome profiles and exit
  -h, --help                Show help message
```

### Examples

```bash
# Check every 30 seconds
node index.js --interval 30

# Protect an extra city (Mumbai/Navi Mumbai/Bergen/Panvel stay protected regardless)
node index.js --allow "oslo"

# Use the other Chrome profile
node index.js --chrome-profile pixofynorge@gmail.com

# Attach to your own Chrome started with a debug port
npm run chrome:debug      # in another terminal, with Chrome closed
node index.js --cdp 9222

# Run headless in the background (once signed in)
node index.js --headless &
```

---

## Running in the Background (macOS)

To keep the bot running quietly in the background on your Mac:

```bash
# Using nohup:
nohup node index.js > bot.log 2>&1 &

# Check logs anytime:
tail -f bot.log

# Stop the bot:
pkill -f "node index.js"
```

Or using [PM2](https://pm2.keymetrics.io/):
```bash
npm install -g pm2
pm2 start index.js --name "claude-session-bot"
pm2 logs claude-session-bot
```

---

## Python Version

```bash
# Install dependencies
pip install -r requirements.txt
playwright install chromium

# Run the 1-minute bot
python3 main.py

# Run with custom 30s interval
python3 main.py --interval 30
```
