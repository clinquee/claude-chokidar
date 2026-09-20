#!/usr/bin/env node

/**
 * Claude Active Sessions Manager (Playwright) - Every-Minute Bot
 *
 * Continuously monitors active Claude sessions and terminates any session
 * not located in a protected location. Runs on the locally installed Chrome
 * (chrome.exe), never on Playwright's bundled Chromium.
 *
 * Safety model: a session is terminated only when its location is read
 * successfully AND is not protected. Anything unclear - blank location,
 * unreadable row, the current session, a table that looks wrong - is kept.
 */

const { chromium } = require('playwright');
const path = require('path');
const http = require('http');
const fs = require('fs');
const { spawn, execFileSync } = require('child_process');
const chromeProfile = require('./chromeProfile');

// Locations that are ALWAYS protected. These can never be removed by --allow;
// --allow only adds more. Matching is case-insensitive substring matching, so
// "Mumbai, Maharashtra, IN" and "Bergen, Vestland, NO" both match.
const PROTECTED_LOCATIONS = ['mumbai', 'navi mumbai', 'bergen', 'panvel'];

// Chrome profile used by default (matched against profile dir, display name or
// account email in Chrome's "Local State").
const DEFAULT_CHROME_PROFILE = 'shreyans.tatiya@gmail.com';

// Parse command line arguments
function parseArgs() {
  const args = process.argv.slice(2);
  const options = {
    dryRun: false,
    allowedLocations: [...PROTECTED_LOCATIONS],
    cdpPort: null,
    userDataDir: path.resolve(__dirname, 'chrome_session'),
    headless: false,
    interval: 60, // Default 60 seconds (1 minute)
    once: false,
    help: false,
    chromePath: null,                        // Path to chrome.exe (auto-detected)
    chromeProfile: DEFAULT_CHROME_PROFILE,   // Which local Chrome profile to use
    useRealProfile: true,                    // Drive the real Chrome profile when possible
    sessionKey: null,                        // claude.ai sessionKey cookie to reuse
    listProfiles: false,                     // Print local Chrome profiles and exit
    botStartedAt: Date.now()
  };

  for (let i = 0; i < args.length; i++) {
    const arg = args[i];
    if (arg === '--dry-run') {
      options.dryRun = true;
    } else if (arg === '--once') {
      options.once = true;
    } else if (arg === '--interval' || arg === '-i') {
      const val = parseInt(args[++i], 10);
      if (!isNaN(val) && val > 0) {
        options.interval = val;
      }
    } else if (arg.startsWith('--interval=')) {
      const val = parseInt(arg.split('=')[1], 10);
      if (!isNaN(val) && val > 0) {
        options.interval = val;
      }
    } else if (arg === '--allow' || arg === '-a') {
      const val = args[++i];
      if (val) {
        // --allow ADDS locations. The protected ones can never be dropped.
        const extra = val.split(',').map(s => s.trim().toLowerCase()).filter(Boolean);
        options.allowedLocations = [...new Set([...PROTECTED_LOCATIONS, ...extra])];
      }
    } else if (arg === '--chrome-path') {
      options.chromePath = args[++i] || null;
    } else if (arg.startsWith('--chrome-path=')) {
      options.chromePath = arg.split('=').slice(1).join('=');
    } else if (arg === '--chrome-profile') {
      options.chromeProfile = args[++i] || DEFAULT_CHROME_PROFILE;
    } else if (arg.startsWith('--chrome-profile=')) {
      options.chromeProfile = arg.split('=').slice(1).join('=');
    } else if (arg === '--session-key') {
      options.sessionKey = args[++i] || null;
    } else if (arg.startsWith('--session-key=')) {
      options.sessionKey = arg.split('=').slice(1).join('=');
    } else if (arg === '--separate-profile' || arg === '--no-real-profile') {
      options.useRealProfile = false;
    } else if (arg === '--cdp') {
      options.cdpPort = args[++i] || '9222';
    } else if (arg.startsWith('--cdp=')) {
      options.cdpPort = arg.split('=')[1];
    } else if (arg === '--user-data-dir') {
      options.userDataDir = path.resolve(args[++i]);
    } else if (arg.startsWith('--user-data-dir=')) {
      options.userDataDir = path.resolve(arg.split('=')[1]);
    } else if (arg === '--headless') {
      options.headless = true;
    } else if (arg === '--list-profiles') {
      options.listProfiles = true;
    } else if (arg === '--help' || arg === '-h') {
      options.help = true;
    }
  }

  return options;
}

function showHelp() {
  console.log(`
Claude Active Sessions Terminator Bot (Playwright)
==================================================

Usage:
  node index.js [options]

Always-protected locations (can never be terminated, cannot be disabled):
  ${PROTECTED_LOCATIONS.join(', ')}

Options:
  --interval, -i <sec>      Check interval in seconds (default: 60 = 1 minute bot)
  --once                    Run a single scan and exit (disables recurring bot loop)
  --dry-run                 Preview sessions without actually terminating them
  --allow, -a <locations>   Extra protected locations, comma-separated (case-insensitive).
                            These are ADDED to the always-protected list above.
  --chrome-profile <name>   Local Chrome profile: folder name, display name or account
                            email (default: "${DEFAULT_CHROME_PROFILE}")
  --chrome-path <exe>       Path to chrome.exe (default: auto-detected local install)
  --session-key <value>     Reuse your existing claude.ai session cookie instead of
                            signing in again (also read from .env / session-key.txt)
  --separate-profile        Skip the real Chrome profile; use ./chrome_session only
  --cdp [port]              Attach to a Chrome already running with remote debugging (e.g. 9222)
  --user-data-dir <dir>     Directory to persist browser login data (default: ./chrome_session)
  --headless                Run in headless mode (recommended after initial login is saved)
  --list-profiles           List the local Chrome profiles and exit
  -h, --help                Show this help message

Examples:
  # 1. Start the every-minute monitoring bot (Default: checks every 60s):
  node index.js

  # 2. Start the bot in Dry-Run mode (Monitor only, no terminations):
  node index.js --dry-run

  # 3. Choose a scan interval (seconds):
  node index.js --interval 60     # every 1 minute   (npm run bot:1m)
  node index.js --interval 300    # every 5 minutes  (npm run bot:5m)
  node index.js --interval 600    # every 10 minutes (npm run bot:10m)
  node index.js --interval 900    # every 15 minutes (npm run bot:15m)

  # 4. Single-run scan and exit:
  node index.js --once

  # 5. Attach to your own Chrome started with a debug port:
  npm run chrome:debug      (in another terminal)
  node index.js --cdp 9222
`);
}

// Claude's account settings page, where the active-session table lives.
const SETTINGS_URL = 'https://claude.ai/settings/account';

// Strict signed-in probe: the login page must not be showing AND the account
// page content must actually be present. A loose check here previously made the
// bot believe a signed-out profile was authenticated.
async function isSignedIn(page) {
  const url = page.url();
  if (url.includes('/login') || url.includes('/magic-link') || url.includes('/onboarding')) return false;
  if ((await page.title().catch(() => '')).includes('Just a moment')) return false;

  const markers = [
    'text=Active sessions',
    'text=Log out of all devices',
    '[data-testid="user-menu-button"]'
  ];
  for (const marker of markers) {
    if (await page.locator(marker).first().isVisible().catch(() => false)) return true;
  }
  return false;
}

// Check if a local port has an active HTTP server (e.g. Chrome Remote Debugging)
function isPortOpen(port) {
  return new Promise(resolve => {
    const req = http.get(`http://127.0.0.1:${port}/json/version`, { timeout: 1000 }, res => {
      resolve(res.statusCode === 200);
    });
    req.on('error', () => resolve(false));
    req.on('timeout', () => {
      req.destroy();
      resolve(false);
    });
  });
}

// Check if a location string matches any of the allowed locations.
// The always-protected list is checked too, even if a caller passes a narrower list.
function isLocationAllowed(location, allowedLocations) {
  if (!location) return false;
  const locLower = location.toLowerCase();
  const list = [...new Set([...PROTECTED_LOCATIONS, ...(allowedLocations || [])])];
  return list.some(allowed => locLower.includes(String(allowed).toLowerCase()));
}

/**
 * Decide what to do with one session row - deliberately biased towards keeping.
 * Returns { keep: boolean, reason: string }.
 */
function classifySession({ deviceText, locationText, isCurrent }, allowedLocations) {
  // The bot's own session is always the one Claude marks "Current": that flag is
  // rendered for whichever session requested the page, which is this browser.
  if (isCurrent) {
    return { keep: true, reason: 'Current session' };
  }

  const location = (locationText || '').replace(/\s+/g, ' ').trim();

  // Fail-safe: a row whose location did not render (slow load, layout change,
  // extra column) must never be terminated - we simply do not know where it is.
  if (!location || location === '-' || location === '--') {
    return { keep: true, reason: 'Location unknown - kept for safety' };
  }

  if (isLocationAllowed(location, allowedLocations)) {
    return { keep: true, reason: 'Protected location' };
  }

  // Equally fail-safe: if the device column is empty the row probably is not a
  // real session row (spacer/header row), so leave it alone.
  if (!(deviceText || '').trim()) {
    return { keep: true, reason: 'Row not readable - kept for safety' };
  }

  return { keep: false, reason: `Location "${location}" is not protected` };
}

// Format timestamp helper
function getTimestamp() {
  return new Date().toLocaleTimeString('en-US', { hour12: false });
}

// Sleep helper
function sleep(ms) {
  return new Promise(resolve => setTimeout(resolve, ms));
}

// Return the first locator in `candidates` that becomes visible, else null.
// Used instead of one comma-joined selector, which Playwright rejects when it
// mixes CSS with a `text=` engine.
async function firstVisible(page, candidates, timeoutMs = 5000) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    for (const candidate of candidates) {
      const first = candidate.first();
      if (await first.isVisible().catch(() => false)) return first;
    }
    await page.waitForTimeout(200);
  }
  return null;
}

// Any logout control is off-limits to the bot. The settings page carries a
// plain "Log out" button (and, on some accounts, "Log out of all devices");
// clicking either would end sessions this bot is meant to protect. Terminating
// a single session is done through the row menu, which never says "log out".
const FORBIDDEN_CLICK_TEXT = /log\s*out|sign\s*out/i;

/**
 * Install a capture-phase guard in the page that swallows any click on a
 * "Log out of all devices" control before it can reach the app.
 *
 * This is the hard stop: even a stray or mis-aimed click cannot trigger it.
 * It applies to this automated browser window only.
 */
async function installLogoutGuard(page) {
  const guard = () => {
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
  };

  // addInitScript covers every future navigation; evaluate covers the page
  // that is already open.
  await page.addInitScript(guard).catch(() => {});
  await page.evaluate(guard).catch(() => {});
}

// Click only after checking the element is not a "log out of all devices" control.
async function safeClick(locator, what = 'element') {
  const label = ((await locator.innerText().catch(() => '')) + ' ' +
                 (await locator.getAttribute('aria-label').catch(() => '') || '')).trim();
  if (FORBIDDEN_CLICK_TEXT.test(label)) {
    throw new Error(`Refused to click ${what}: its label ("${label}") would log out all devices.`);
  }
  await locator.click();
}

/**
 * Confirm Claude's "Terminate session" dialog.
 *
 * Deliberately strict: it clicks a button only when that button's own label is
 * exactly "Terminate". The settings page also carries a "Log out of all
 * devices" button, and a loose text match there would sign out every session,
 * protected ones included - so no such fallback exists here.
 */
async function confirmTermination(page, timeoutMs = 8000) {
  // Target the confirmation modal specifically: it is an alertdialog headed
  // "Terminate session". The settings panel itself is also role="dialog", so an
  // unfiltered lookup would land on the wrong element.
  const dialog = await firstVisible(page, [
    page.locator('[role="alertdialog"]').filter({ hasText: /terminate session/i }),
    page.getByRole('alertdialog'),
    page.locator('[role="dialog"]').filter({ hasText: /terminate session/i })
  ], timeoutMs);

  if (!dialog) return false;

  const button = await firstVisible(page, [
    dialog.getByRole('button', { name: /^\s*terminate\s*$/i }),
    dialog.locator('button').filter({ hasText: /^\s*terminate\s*$/i })
  ], 4000);

  if (!button) return false;

  const label = (await button.innerText().catch(() => '')).trim();
  if (!/^terminate$/i.test(label)) {
    console.warn(`  --> Refusing to click confirm button labelled "${label}".`);
    return false;
  }

  await safeClick(button, 'the confirm button');
  return true;
}

// Close a dialog that was opened but must not be acted on.
async function dismissDialog(page) {
  await page.keyboard.press('Escape').catch(() => {});
  await page.waitForTimeout(300);
}

// Read every session row's text in one pass (device, location, created).
async function readAllRows(page) {
  const rows = await page.locator('table tr').all();
  const out = [];
  for (let i = 1; i < rows.length; i++) {
    const cells = await rows[i].locator('th, td').all();
    if (cells.length < 2) continue;
    out.push({
      deviceText: (await cells[0].innerText().catch(() => '')).trim(),
      locationText: (await cells[1].innerText().catch(() => '')).trim(),
      createdText: cells.length >= 3 ? (await cells[2].innerText().catch(() => '')).trim() : ''
    });
  }
  return out;
}

// Re-read a row's cells straight from the live DOM (used right before acting).
async function rereadRow(row) {
  const cells = await row.locator('th, td').all();
  if (cells.length < 2) return null;
  const deviceText = (await cells[0].innerText().catch(() => '')).trim();
  const locationText = (await cells[1].innerText().catch(() => '')).trim();
  const isCurrent = deviceText.toLowerCase().includes('current') ||
                    (await row.locator('text=Current').count()) > 0 ||
                    (await row.locator('button[aria-label*="current session"]').count()) > 0;
  return { deviceText, locationText, isCurrent };
}

// Perform a single scan and termination pass
async function performScan(page, options, checkNumber) {
  const settingsUrl = SETTINGS_URL;
  const time = getTimestamp();

  console.log(`\n[${time}] Check #${checkNumber}: Inspecting active sessions...`);

  // Ensure settings account page is loaded and fresh
  try {
    const currentUrl = page.url();
    if (!currentUrl.includes('/settings/account')) {
      await page.goto(settingsUrl, { waitUntil: 'domcontentloaded', timeout: 30000 });
    } else {
      // Reload page to get fresh session data from server
      await page.reload({ waitUntil: 'domcontentloaded', timeout: 30000 });
    }
  } catch (e) {
    console.warn(`[${time}] Navigation warning: ${e.message}`);
  }

  // Re-arm the guard after every navigation/reload.
  await installLogoutGuard(page);

  // Ensure "Active sessions" is visible
  let activeSessionsHeader = page.locator('text=Active sessions');
  if (!(await activeSessionsHeader.isVisible().catch(() => false))) {
    await page.goto(settingsUrl, { waitUntil: 'domcontentloaded', timeout: 30000 });
    await page.waitForTimeout(2000);
  }

  try {
    await page.waitForSelector('text=Active sessions', { timeout: 15000 });
  } catch (err) {
    console.error(`[${time}] Could not find "Active sessions" table. Re-checking on next interval.`);
    return { scanned: 0, kept: 0, terminated: 0 };
  }

  // Scroll down container to load any deferred session items
  try {
    await page.evaluate(() => {
      const scrollables = document.querySelectorAll('div[class*="overflow-y-auto"]');
      scrollables.forEach(s => { s.scrollTop = s.scrollHeight; });
    });
    await page.waitForTimeout(1000);
  } catch (e) {}

  let terminatedCount = 0;
  let keptCount = 0;
  const processedKeys = new Set();

  let continueScanning = true;
  while (continueScanning) {
    continueScanning = false;

    const rows = await page.locator('table tr').all();
    if (rows.length <= 1) {
      break;
    }

    let rowToTerminate = null;
    let sessionDetails = null;
    let readableRows = 0;
    let keepableRows = 0;

    for (let i = 1; i < rows.length; i++) {
      const row = rows[i];
      const cells = await row.locator('th, td').all();
      if (cells.length < 2) continue;

      const deviceText = (await cells[0].innerText().catch(() => '')).trim();
      const locationText = (await cells[1].innerText().catch(() => '')).trim();
      const createdText = cells.length >= 3 ? (await cells[2].innerText().catch(() => '')).trim() : '';

      const isCurrent = deviceText.toLowerCase().includes('current') ||
                        (await row.locator('text=Current').count()) > 0 ||
                        (await row.locator('button[aria-label*="current session"]').count()) > 0;

      const sessionKey = `${deviceText}_${locationText}_${createdText}`;
      const verdict = classifySession({ deviceText, locationText, isCurrent }, options.allowedLocations);

      readableRows++;
      if (verdict.keep) keepableRows++;

      if (verdict.keep) {
        if (!processedKeys.has(sessionKey)) {
          console.log(`  [KEEP]  ${deviceText.replace(/\n/g, ' ')} | Location: ${locationText || 'N/A'} | ${verdict.reason}`);
          processedKeys.add(sessionKey);
          keptCount++;
        }
        continue;
      }

      // Unauthorized session found - remember it, but only act after the whole
      // table has been read, so the sanity check below can veto it.
      if (!rowToTerminate) {
        rowToTerminate = row;
        sessionDetails = { deviceText, locationText, createdText, sessionKey, reason: verdict.reason };
      }
    }

    // Runaway guard: on a healthy account at least the current session is kept.
    // If the page says every single row should go, the table was most likely
    // misread (layout change, half-rendered page) - do nothing this round.
    if (rowToTerminate && readableRows > 1 && keepableRows === 0) {
      console.warn('  [ABORT] Every row looked unprotected, which is almost certainly a misread page. Nothing terminated.');
      break;
    }

    if (rowToTerminate && sessionDetails) {
      const { deviceText, locationText, createdText, sessionKey } = sessionDetails;

      if (options.dryRun) {
        console.log(`  [WOULD TERMINATE] ${deviceText} | Location: ${locationText || 'Unknown'} | Created: ${createdText}`);
        processedKeys.add(sessionKey);
        terminatedCount++;
        // In dry run, move on to remaining rows without terminating
        continueScanning = false;
      } else {
        // Last-moment re-read: the table may have re-rendered between reading
        // the row and acting on it. Only proceed if the row still says the same
        // thing and still classifies as unprotected.
        const recheck = await rereadRow(rowToTerminate).catch(() => null);
        if (!recheck) {
          console.warn('  [SKIP] Row vanished before termination - re-scanning next round.');
          continueScanning = true;
          continue;
        }
        const recheckVerdict = classifySession(recheck, options.allowedLocations);
        if (recheckVerdict.keep || recheck.locationText !== locationText || recheck.deviceText !== deviceText) {
          console.warn(`  [SKIP] Row changed on re-check (now "${recheck.deviceText}" @ "${recheck.locationText}" - ${recheckVerdict.reason}). Not terminating.`);
          processedKeys.add(sessionKey);
          keptCount++;
          continueScanning = true;
          continue;
        }

        console.log(`  [TERMINATING] ${deviceText} | Location: ${locationText || 'Unknown'} | Created: ${createdText}...`);

        try {
          const actionBtn = rowToTerminate.locator('button[aria-haspopup="menu"], button[aria-label*="Session actions"], button').first();
          await actionBtn.scrollIntoViewIfNeeded();
          await safeClick(actionBtn, 'the row action button');
          await page.waitForTimeout(500);

          // Playwright rejects a selector list that mixes CSS with a `text=`
          // engine, so each candidate is tried as its own locator.
          const terminateItem = await firstVisible(page, [
            page.getByRole('menuitem', { name: /terminate/i }),
            page.locator('[role="menu"] button').filter({ hasText: /terminate/i }),
            page.locator('[role="menuitem"]:has-text("Terminate")'),
            page.locator('[data-part="item"]:has-text("Terminate")'),
            page.getByText('Terminate', { exact: false })
          ], 5000);

          if (!terminateItem) {
            throw new Error('No "Terminate" menu item appeared.');
          }
          await safeClick(terminateItem, 'the Terminate menu item');
          await page.waitForTimeout(500);

          // Claude asks "Terminate session - are you sure?" before acting.
          const confirmed = await confirmTermination(page);
          if (!confirmed) {
            console.warn('  --> Could not confirm in the dialog; nothing was terminated.');
            await dismissDialog(page);
            continueScanning = true;
            continue;
          }

          // Only count it once the row is actually gone from the table.
          await page.waitForTimeout(1500);
          const stillThere = (await readAllRows(page)).some(r =>
            r.deviceText === deviceText && r.locationText === locationText && r.createdText === createdText);

          if (stillThere) {
            console.warn('  --> Session still listed after confirming; will retry next round.');
          } else {
            console.log('  --> Successfully terminated session!');
            terminatedCount++;
          }

          // Re-evaluate table on next iteration
          continueScanning = true;
        } catch (termErr) {
          console.error(`  --> Error terminating session: ${termErr.message}`);
        }
      }
    }
  }

  // Finish collecting any remaining rows if in dry run
  if (options.dryRun) {
    const rows = await page.locator('table tr').all();
    for (let i = 1; i < rows.length; i++) {
      const row = rows[i];
      const cells = await row.locator('th, td').all();
      if (cells.length < 2) continue;

      const deviceText = (await cells[0].innerText().catch(() => '')).trim();
      const locationText = (await cells[1].innerText().catch(() => '')).trim();
      const createdText = cells.length >= 3 ? (await cells[2].innerText().catch(() => '')).trim() : '';
      const sessionKey = `${deviceText}_${locationText}_${createdText}`;

      if (processedKeys.has(sessionKey)) continue;

      const isCurrent = deviceText.toLowerCase().includes('current') || (await row.locator('text=Current').count()) > 0;
      const verdict = classifySession({ deviceText, locationText, isCurrent }, options.allowedLocations);

      if (verdict.keep) {
        console.log(`  [KEEP]  ${deviceText.replace(/\n/g, ' ')} | Location: ${locationText || 'N/A'} | ${verdict.reason}`);
        keptCount++;
      } else {
        console.log(`  [WOULD TERMINATE] ${deviceText} | Location: ${locationText || 'Unknown'} | Created: ${createdText}`);
        terminatedCount++;
      }
      processedKeys.add(sessionKey);
    }
  }

  const total = processedKeys.size;
  console.log(`[${time}] Check complete. Total: ${total} | Kept: ${keptCount} | ${options.dryRun ? 'Flagged' : 'Terminated'}: ${terminatedCount}`);

  return { scanned: total, kept: keptCount, terminated: terminatedCount };
}

// Read a saved claude.ai sessionKey (so the bot reuses your existing session
// instead of signing in again and creating another device entry).
/**
 * Parse whatever was pasted into a list of cookies.
 *
 * claude.ai uses several session cookies side by side (sessionKey,
 * sessionKeyV3, and their *LC variants), so rather than guessing which one is
 * authoritative this accepts any of these shapes:
 *   - a bare value                      -> treated as sessionKey
 *   - "sessionKey=abc; sessionKeyV3=de" -> a whole Cookie header
 *   - one "name=value" (or "name<TAB>value") per line
 */
function parseSessionCookies(raw) {
  if (!raw) return [];
  // Strip a UTF-16/UTF-8 BOM: PowerShell's ">" redirection writes UTF-16 files.
  const text = String(raw).replace(/^﻿/, '').replace(/\u0000/g, '');

  const chunks = text
    .split(/[\r\n;]+/)
    .map(part => part.trim())
    .filter(part => part && !part.startsWith('#'));

  const cookies = [];
  for (const chunk of chunks) {
    const match = chunk.match(/^["']?([A-Za-z0-9_.\-]+)["']?\s*[=\t:]\s*["']?(.+?)["']?$/);
    if (match) {
      cookies.push({ name: match[1], value: match[2].trim() });
    } else if (!/\s/.test(chunk)) {
      // A bare value with no name: that is the classic sessionKey cookie.
      cookies.push({ name: 'sessionKey', value: chunk.replace(/^["']|["']$/g, '') });
    }
  }

  // Keep only session cookies, last occurrence of each name wins.
  const byName = new Map();
  for (const cookie of cookies) {
    if (!/^sessionKey/i.test(cookie.name)) continue;
    if (cookie.value) byName.set(cookie.name, cookie.value);
  }
  return [...byName.entries()].map(([name, value]) => ({ name, value }));
}

// Minimal .env reader (no dependency): KEY=value lines, # comments, optional quotes.
function loadDotEnv(file) {
  const env = {};
  if (!fs.existsSync(file)) return env;
  const text = fs.readFileSync(file, 'utf8').replace(/^﻿/, '');
  for (const line of text.split(/\r?\n/)) {
    const trimmed = line.trim();
    if (!trimmed || trimmed.startsWith('#')) continue;
    const eq = trimmed.indexOf('=');
    if (eq < 1) continue;
    const key = trimmed.slice(0, eq).trim().replace(/^export\s+/, '');
    const value = trimmed.slice(eq + 1).trim().replace(/^["']|["']$/g, '');
    if (key && value) env[key] = value;
  }
  return env;
}

/**
 * Map a .env key to the claude.ai cookie it holds:
 *   SESSION_KEY        -> sessionKey
 *   SESSION_KEYLC      -> sessionKeyLC
 *   SESSION_KEYV3      -> sessionKeyV3
 *   SESSION_KEYV3LC    -> sessionKeyV3LC
 * A trailing _v2 / _2 marks a second value for the same cookie name (claude.ai
 * lists some of these twice, once host-only and once domain-wide).
 */
function cookieNameFromEnvKey(key) {
  const match = key.match(/^SESSION_KEY([A-Za-z0-9]*?)(?:_v?\d+)?$/i);
  if (!match) return null;
  return 'sessionKey' + (match[1] || '').toUpperCase();
}

function cookiesFromEnv(env) {
  const cookies = [];
  for (const [key, value] of Object.entries(env)) {
    const name = cookieNameFromEnvKey(key);
    if (name && value) cookies.push({ name, value, envKey: key });
  }
  return cookies;
}

// Returns the cookies to inject, or [] when nothing has been configured.
// Order of precedence: --session-key, CLAUDE_SESSION_KEY, session-key.txt, .env.
function readSessionCookies(explicit) {
  if (explicit) return parseSessionCookies(explicit);
  if (process.env.CLAUDE_SESSION_KEY) return parseSessionCookies(process.env.CLAUDE_SESSION_KEY);

  const file = path.join(__dirname, 'session-key.txt');
  if (fs.existsSync(file)) {
    const fromFile = parseSessionCookies(fs.readFileSync(file, 'utf8'));
    if (fromFile.length) return fromFile;
  }

  const fromDotEnv = cookiesFromEnv(loadDotEnv(path.join(__dirname, '.env')));
  if (fromDotEnv.length) return fromDotEnv;

  return cookiesFromEnv(process.env);
}

// Backwards-compatible single-key accessor.
function readSessionKey(explicit) {
  const cookies = readSessionCookies(explicit);
  return cookies.length ? cookies[0].value : null;
}

async function applySessionCookies(context, cookies) {
  const expires = Math.floor(Date.now() / 1000) + 60 * 60 * 24 * 365;

  // claude.ai lists some of these cookies twice - once host-only (claude.ai)
  // and once domain-wide (.claude.ai). When two values are supplied for one
  // name, the first takes the domain-wide slot and the second the host-only
  // one; a single value is written to both so the shape cannot be wrong.
  const byName = new Map();
  for (const { name, value } of cookies) {
    if (!byName.has(name)) byName.set(name, []);
    byName.get(name).push(value);
  }

  const payload = [];
  for (const [name, values] of byName) {
    const domains = values.length > 1 ? ['.claude.ai', 'claude.ai'] : ['.claude.ai', 'claude.ai'];
    values.slice(0, 2).forEach((value, i) => {
      const targets = values.length > 1 ? [domains[i]] : domains;
      for (const domain of targets) {
        payload.push({ name, value, domain, path: '/', httpOnly: true, secure: true, sameSite: 'Lax', expires });
      }
    });
  }

  await context.addCookies(payload);
}

const REAL_PROFILE_DEBUG_PORT = 9223;

/**
 * Start the locally installed Chrome on the user's real profile with a debug
 * port and attach to it. Returns null (after cleaning up) when Chrome refuses,
 * which is what Chrome 136+ does for the default user-data-dir.
 */
async function tryLaunchRealProfile({ chromePath, liveUserDataDir, profile, timeoutMs = 20000 }) {
  console.log(`Starting your Chrome on profile "${profile.dir}" with debugging enabled...`);

  const child = spawn(chromePath, [
    `--remote-debugging-port=${REAL_PROFILE_DEBUG_PORT}`,
    `--user-data-dir=${liveUserDataDir}`,
    `--profile-directory=${profile.dir}`,
    '--no-first-run',
    '--no-default-browser-check',
    SETTINGS_URL
  ], { detached: true, stdio: 'ignore' });
  child.unref();

  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    if (await isPortOpen(REAL_PROFILE_DEBUG_PORT)) {
      const browser = await chromium.connectOverCDP(`http://127.0.0.1:${REAL_PROFILE_DEBUG_PORT}`);
      const contexts = browser.contexts();
      const context = contexts.length > 0 ? contexts[0] : await browser.newContext();
      const pages = context.pages();
      let page = pages.find(p => p.url().includes('claude.ai'));
      if (page) {
        await page.bringToFront().catch(() => {});
      } else {
        page = await context.newPage();
      }
      return { browser, context, page };
    }
    await sleep(500);
  }

  // Debugging never came up: close the window we opened so nothing is left behind.
  try {
    if (process.platform === 'win32') {
      execFileSync('taskkill', ['/PID', String(child.pid), '/T', '/F'], { stdio: 'ignore' });
    } else {
      process.kill(-child.pid, 'SIGTERM');
    }
  } catch (e) {}
  return null;
}

/**
 * Open a browser for the bot, preferring the user's own Chrome install.
 *
 * Order of preference:
 *   1. Attach over CDP to a Chrome the user already has running with a debug
 *      port - that is literally their live browser and session.
 *   2. Start the local chrome.exe on their real profile with a debug port and
 *      attach to that (only works when Chrome is closed, and only on Chrome
 *      builds that still allow debugging on the default user-data-dir).
 *   3. Launch the locally installed chrome.exe against the bot's own profile
 *      directory, seeded with the claude.ai sessionKey when one is provided.
 *
 * Note on why the live Chrome profile cannot simply be borrowed: Chrome 136+
 * refuses remote debugging on the default user-data-dir, and Chrome's
 * app-bound cookie encryption (v20) makes a copied profile decrypt to nothing.
 * So either attach over CDP, or sign in once in the bot's own profile.
 */
async function openBrowser(options) {
  const chromePath = chromeProfile.findChromeExecutable(options.chromePath);
  const result = { browser: null, context: null, page: null, isCdp: false, chromePath };

  const cdpPort = options.cdpPort || (await isPortOpen(9222) ? '9222' : null);
  if (cdpPort) {
    const cdpUrl = `http://127.0.0.1:${cdpPort}`;
    console.log(`\nAttaching to your running Chrome over CDP (${cdpUrl})...`);
    result.browser = await chromium.connectOverCDP(cdpUrl);
    const contexts = result.browser.contexts();
    result.context = contexts.length > 0 ? contexts[0] : await result.browser.newContext();

    const pages = result.context.pages();
    result.page = pages.find(p => p.url().includes('claude.ai'));
    if (result.page) {
      console.log(`Found an existing Claude tab: ${result.page.url()}`);
      await result.page.bringToFront();
    } else {
      result.page = await result.context.newPage();
    }
    result.isCdp = true;
    return result;
  }

  if (!chromePath) {
    console.log('\nCould not find a local Chrome install; falling back to Playwright Chromium.');
    console.log('Pass --chrome-path "C:\\Path\\To\\chrome.exe" to point at it explicitly.');
  } else {
    console.log(`\nUsing your local Chrome: ${chromePath}`);
  }

  // Report which local profile we are targeting, so the right account is used.
  const liveUserDataDir = chromeProfile.defaultUserDataDir();
  const profile = chromeProfile.resolveProfile(liveUserDataDir, options.chromeProfile);
  if (profile) {
    console.log(`Chrome account    : ${profile.email || profile.name} (${profile.dir})`);
  } else if (options.chromeProfile) {
    console.log(`Chrome account    : no local profile matched "${options.chromeProfile}"`);
  }

  // Best case: start YOUR Chrome, on YOUR profile, with a debug port, and drive
  // that. Only possible while Chrome is not already running (its profile is
  // single-instance locked), and only if this Chrome build still allows remote
  // debugging on the default user-data-dir.
  if (options.useRealProfile && chromePath && profile) {
    if (chromeProfile.isChromeRunning()) {
      console.log('Chrome is already running, so it cannot be switched into debug mode.');
      console.log('Close Chrome and re-run to drive your real profile, or run "npm run chrome:debug" first.');
    } else {
      const attached = await tryLaunchRealProfile({ chromePath, liveUserDataDir, profile });
      if (attached) {
        console.log('Attached to your real Chrome profile - your existing claude.ai login is in use.');
        return { ...result, ...attached, isCdp: true };
      }
      console.log('This Chrome build refuses remote debugging on the default profile directory');
      console.log('(Chrome 136+ security change), so falling back to the bot profile.');
    }
  }

  if (!fs.existsSync(options.userDataDir)) {
    fs.mkdirSync(options.userDataDir, { recursive: true });
  }

  const launchOptions = {
    headless: options.headless,
    viewport: { width: 1280, height: 850 },
    args: [
      '--disable-blink-features=AutomationControlled',
      '--no-default-browser-check',
      '--no-first-run'
    ]
  };

  if (chromePath) {
    result.context = await chromium.launchPersistentContext(options.userDataDir, {
      ...launchOptions,
      executablePath: chromePath
    });
  } else {
    try {
      result.context = await chromium.launchPersistentContext(options.userDataDir, {
        ...launchOptions,
        channel: 'chrome'
      });
    } catch (err) {
      result.context = await chromium.launchPersistentContext(options.userDataDir, launchOptions);
    }
  }

  const sessionCookies = readSessionCookies(options.sessionKey);
  if (sessionCookies.length) {
    console.log(`Reusing your existing claude.ai session (${sessionCookies.map(c => c.name).join(', ')}) - no new sign-in.`);
    await applySessionCookies(result.context, sessionCookies);
  }

  result.page = result.context.pages().length > 0 ? result.context.pages()[0] : await result.context.newPage();
  return result;
}

async function main() {
  const options = parseArgs();

  if (options.help) {
    showHelp();
    return;
  }

  if (options.listProfiles) {
    const userDataDir = chromeProfile.defaultUserDataDir();
    const { profiles, lastUsed } = chromeProfile.listProfiles(userDataDir);
    console.log(`Chrome user data: ${userDataDir}`);
    if (!profiles.length) {
      console.log('No Chrome profiles found.');
      return;
    }
    for (const p of profiles) {
      const marks = [p.dir === lastUsed ? 'last used' : null,
                     chromeProfile.isProfileLocked(userDataDir, p.dir) ? 'open in Chrome' : null]
                    .filter(Boolean).join(', ');
      console.log(`  ${p.dir.padEnd(12)} ${(p.name || '').padEnd(14)} ${(p.email || '').padEnd(30)} ${marks}`);
    }
    return;
  }

  console.log('='.repeat(65));
  console.log('  Claude Active Sessions Manager - Bot Mode');
  console.log('='.repeat(65));
  console.log(`Protected (always): ${PROTECTED_LOCATIONS.join(', ')}`);
  const extras = options.allowedLocations.filter(l => !PROTECTED_LOCATIONS.includes(l));
  if (extras.length) console.log(`Protected (extra) : ${extras.join(', ')}`);
  console.log(`Mode              : ${options.dryRun ? 'DRY-RUN (Preview only)' : 'LIVE (Auto-terminate unprotected)'}`);
  console.log(`Interval          : ${options.once ? 'Single scan (--once)' : `Every ${options.interval}s (Every-minute bot)`}`);
  console.log(`Session Dir       : ${options.userDataDir}`);

  console.log('='.repeat(65));

  let browser = null;
  let context = null;
  let page = null;
  let isCdp = false;
  let isShuttingDown = false;

  // Graceful shutdown handler
  async function cleanup() {
    if (isShuttingDown) return;
    isShuttingDown = true;
    console.log('\n\nStopping bot and cleaning up browser session...');
    try {
      if (context && !isCdp) {
        await context.close().catch(() => {});
      } else if (browser && isCdp) {
        await browser.close().catch(() => {});
      }
    } catch (e) {}
    console.log('Bot shutdown cleanly. Bye!');
    process.exit(0);
  }

  process.on('SIGINT', cleanup);
  process.on('SIGTERM', cleanup);

  try {
    // 1. Open the browser (attach to your Chrome, or launch your chrome.exe)
    const opened = await openBrowser(options);
    browser = opened.browser;
    context = opened.context;
    page = opened.page;
    isCdp = opened.isCdp;

    // 2. Initial Authentication Check
    const settingsUrl = SETTINGS_URL;
    console.log(`Navigating to ${settingsUrl}...`);
    await page.goto(settingsUrl, { waitUntil: 'domcontentloaded', timeout: 45000 });

    console.log('Checking authentication status...');
    let loggedIn = false;
    let promptedForLogin = false;
    const maxWaitTime = 300000; // 5 minutes to finish a manual sign-in
    const startTime = Date.now();

    while (Date.now() - startTime < maxWaitTime && !isShuttingDown) {
      if (await isSignedIn(page)) {
        loggedIn = true;
        break;
      }

      const currentUrl = page.url();
      const onLoginPage = currentUrl.includes('/login') || currentUrl.includes('/magic-link');
      const onTurnstile = (await page.title().catch(() => '')).includes('Just a moment');

      if ((onLoginPage || onTurnstile) && !promptedForLogin) {
        promptedForLogin = true;
        console.log('');
        if (readSessionKey(options.sessionKey)) {
          console.log('[Action required] The saved session key was not accepted - it has most');
          console.log('likely expired, or was truncated when it was copied.');
          console.log('Re-copy it from your everyday Chrome (F12 > Application > Cookies >');
          console.log('https://claude.ai > sessionKey) and save it with:');
          console.log('  npm run session-key -- <paste the value>');
          console.log('Or just sign in in the window that opened; that also works.');
        } else {
          console.log('[Action required] This browser profile is not signed in to Claude.');
          console.log('Sign in once in the window that just opened - it is remembered from then on.');
          console.log('To avoid a sign-in entirely, copy the "sessionKey" cookie out of your');
          console.log('everyday Chrome (F12 > Application > Cookies > https://claude.ai) and run:');
          console.log('  npm run session-key -- <paste the value>');
        }
        console.log('');
      }

      if (onLoginPage) {
        // Wait for the sign-in to land somewhere that is not the login flow.
        await page.waitForURL(url => !url.toString().includes('/login'), {
          timeout: Math.max(5000, maxWaitTime - (Date.now() - startTime))
        }).catch(() => {});
        await page.goto(settingsUrl, { waitUntil: 'domcontentloaded', timeout: 45000 }).catch(() => {});
      }

      await page.waitForTimeout(2000);
    }

    if (!loggedIn) {
      throw new Error('Not signed in to Claude (timed out waiting). Sign in once in the bot window, or provide a session key, then restart.');
    }

    // Anchor timing and make "Log out of all devices" unclickable in this window.
    options.botStartedAt = Date.now();
    await installLogoutGuard(page);
    console.log('Safety guard armed: every "Log out" control is blocked in this window.');

    console.log('Authentication confirmed! Bot is active.');

    // 3. Bot Loop
    let checkCount = 1;
    while (!isShuttingDown) {
      try {
        await performScan(page, options, checkCount++);
      } catch (scanErr) {
        console.error(`[${getTimestamp()}] Scan error: ${scanErr.message}. Will retry on next interval.`);
      }

      if (options.once) {
        console.log('\nSingle scan completed (--once). Exiting.');
        break;
      }

      console.log(`\n⏳ Next check in ${options.interval}s (Press Ctrl+C to stop)...`);
      await sleep(options.interval * 1000);
    }

  } catch (error) {
    if (!isShuttingDown) {
      console.error('\n[Error occurred during bot execution]:', error.message);
    }
  } finally {
    if (!isShuttingDown) {
      await cleanup();
    }
  }
}

if (require.main === module) {
  main().catch(err => {
    console.error('Fatal error:', err);
    process.exit(1);
  });
}

module.exports = {
  main,
  isLocationAllowed,
  classifySession,
  performScan,
  parseSessionCookies,
  readSessionCookies,
  installLogoutGuard,
  safeClick,
  confirmTermination,
  PROTECTED_LOCATIONS
};
