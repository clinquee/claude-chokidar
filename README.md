# Claude Active Sessions Terminator Bot (Playwright)

An automated Playwright bot that runs continuously, checking your Claude account's **Active Sessions** every minute (or custom interval) and automatically terminating/logging out any sessions that are **not** from allowed locations.

### Allowed Locations Policy
- ✅ **Mumbai** (Preserved)
- ✅ **Navi Mumbai** (Preserved)
- ✅ **Bergen** (Preserved)
- ✅ **Panvel** (Preserved)
- ✅ **Current Session** (Always preserved to prevent accidental self-logout)
- ❌ **All other locations** (Automatically terminated on sight)

---

## How the Bot Works

1. **Initial Run**: Launches desktop Chrome with a persistent profile (`./chrome_session`). You log in once if prompted; your session is permanently saved.
2. **Every Minute**: The bot checks `https://claude.ai/new#settings/account`, refreshes the active sessions table, and scans all connected devices.
3. **Automatic Termination**: Any unauthorized session (outside Mumbai, Navi Mumbai, Bergen) is clicked and terminated.
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
  --allow, -a <locations>   Comma-separated list of allowed locations (case-insensitive)
                            Default: "mumbai,navi mumbai,bergen"
  --cdp [port]              Connect to an already running browser (e.g. 9222)
  --user-data-dir <dir>     Directory to persist browser login data (default: ./chrome_session)
  --headless                Run in headless mode (after initial login is saved)
  -h, --help                Show help message
```

### Examples

```bash
# Check every 30 seconds
node index.js --interval 30

# Custom locations (e.g. add Oslo)
node index.js --allow "mumbai,navi mumbai,bergen,oslo"

# Run headless in the background (once logged in)
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
