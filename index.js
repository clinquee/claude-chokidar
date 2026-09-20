#!/usr/bin/env node

/**
 * Claude Active Sessions Manager (Playwright) - Every-Minute Bot
 *
 * Continuously monitors active Claude sessions and terminates any session
 * not located in allowed locations (default: Mumbai, Navi Mumbai, Bergen).
 * Never terminates the current active session.
 */

const { chromium } = require('playwright');
const path = require('path');
const http = require('http');
const fs = require('fs');

// Default allowed locations (case-insensitive substring match)
const DEFAULT_ALLOWED_LOCATIONS = ['mumbai', 'navi mumbai', 'bergen', 'panvel'];

// Parse command line arguments
function parseArgs() {
  const args = process.argv.slice(2);
  const options = {
    dryRun: false,
    allowedLocations: [...DEFAULT_ALLOWED_LOCATIONS],
    cdpPort: null,
    userDataDir: path.resolve(__dirname, 'chrome_session'),
    headless: false,
    interval: 60, // Default 60 seconds (1 minute)
    once: false,
    help: false
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
        options.allowedLocations = val.split(',').map(s => s.trim().toLowerCase()).filter(Boolean);
      }
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

Options:
  --interval, -i <sec>      Check interval in seconds (default: 60 = 1 minute bot)
  --once                    Run a single scan and exit (disables recurring bot loop)
  --dry-run                 Preview sessions without actually terminating them
  --allow, -a <locations>   Comma-separated list of allowed locations (case-insensitive)
                            Default: "mumbai,navi mumbai,bergen,panvel"
  --cdp [port]              Connect to an already running browser with remote debugging (e.g. 9222)
  --user-data-dir <dir>     Directory to persist browser login data (default: ./chrome_session)
  --headless                Run in headless mode (recommended after initial login is saved)
  -h, --help                Show this help message

Examples:
  # 1. Start the every-minute monitoring bot (Default: checks every 60s):
  node index.js

  # 2. Start the bot in Dry-Run mode (Monitor only, no terminations):
  node index.js --dry-run

  # 3. Check every 30 seconds:
  node index.js --interval 30

  # 4. Single-run scan and exit:
  node index.js --once

  # 5. Connect to already running Chrome on port 9222:
  node index.js --cdp 9222
`);
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

// Check if a location string matches any of the allowed locations
function isLocationAllowed(location, allowedLocations) {
  if (!location) return false;
  const locLower = location.toLowerCase();
  return allowedLocations.some(allowed => locLower.includes(allowed.toLowerCase()));
}

// Format timestamp helper
function getTimestamp() {
  return new Date().toLocaleTimeString('en-US', { hour12: false });
}

// Sleep helper
function sleep(ms) {
  return new Promise(resolve => setTimeout(resolve, ms));
}

// Perform a single scan and termination pass
async function performScan(page, options, checkNumber) {
  const settingsUrl = 'https://claude.ai/new#settings/account';
  const time = getTimestamp();

  console.log(`\n[${time}] Check #${checkNumber}: Inspecting active sessions...`);

  // Ensure settings account page is loaded and fresh
  try {
    const currentUrl = page.url();
    if (!currentUrl.includes('settings/account')) {
      await page.goto(settingsUrl, { waitUntil: 'domcontentloaded', timeout: 30000 });
    } else {
      // Reload page to get fresh session data from server
      await page.reload({ waitUntil: 'domcontentloaded', timeout: 30000 });
    }
  } catch (e) {
    console.warn(`[${time}] Navigation warning: ${e.message}`);
  }

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

      // Always keep current session
      if (isCurrent) {
        if (!processedKeys.has(sessionKey)) {
          console.log(`  [KEEP]  ${deviceText.replace(/\n/g, ' ')} | Location: ${locationText || 'N/A'} (Current Session)`);
          processedKeys.add(sessionKey);
          keptCount++;
        }
        continue;
      }

      // Check allowed locations
      const allowed = isLocationAllowed(locationText, options.allowedLocations);
      if (allowed) {
        if (!processedKeys.has(sessionKey)) {
          console.log(`  [KEEP]  ${deviceText} | Location: ${locationText} | Created: ${createdText}`);
          processedKeys.add(sessionKey);
          keptCount++;
        }
        continue;
      }

      // Unauthorized session found!
      rowToTerminate = row;
      sessionDetails = { deviceText, locationText, createdText, sessionKey };
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
        console.log(`  [TERMINATING] ${deviceText} | Location: ${locationText || 'Unknown'} | Created: ${createdText}...`);

        try {
          const actionBtn = rowToTerminate.locator('button[aria-haspopup="menu"], button[aria-label*="Session actions"], button').first();
          await actionBtn.scrollIntoViewIfNeeded();
          await actionBtn.click();
          await page.waitForTimeout(500);

          const terminateItem = page.locator('[role="menuitem"]:has-text("Terminate"), [role="menu"] button:has-text("Terminate"), [data-part="item"]:has-text("Terminate"), text=Terminate').first();
          await terminateItem.waitFor({ state: 'visible', timeout: 5000 });
          await terminateItem.click();
          await page.waitForTimeout(500);

          // Handle optional confirmation dialog
          try {
            const confirmBtn = page.locator('[role="dialog"] button:has-text("Terminate"), [role="alertdialog"] button:has-text("Terminate"), div[data-cds="Dialog"] button:has-text("Terminate"), button:has-text("Log out")').first();
            if (await confirmBtn.isVisible({ timeout: 2000 })) {
              await confirmBtn.click();
            }
          } catch (e) {}

          console.log(`  --> Successfully terminated session!`);
          terminatedCount++;
          await page.waitForTimeout(1500);

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
      const allowed = isLocationAllowed(locationText, options.allowedLocations);

      if (isCurrent || allowed) {
        console.log(`  [KEEP]  ${deviceText.replace(/\n/g, ' ')} | Location: ${locationText || 'N/A'}`);
        keptCount++;
      } else {
        console.log(`  [WOULD TERMINATE] ${deviceText} | Location: ${locationText || 'Unknown'} | Created: ${createdText}`);
        terminatedCount++;
      }
      processedKeys.add(sessionKey);
    }
  }

  const total = processedKeys.length;
  console.log(`[${time}] Check complete. Total: ${total} | Kept: ${keptCount} | ${options.dryRun ? 'Flagged' : 'Terminated'}: ${terminatedCount}`);

  return { scanned: total, kept: keptCount, terminated: terminatedCount };
}

async function main() {
  const options = parseArgs();

  if (options.help) {
    showHelp();
    return;
  }

  console.log('='.repeat(65));
  console.log('  Claude Active Sessions Manager - Bot Mode');
  console.log('='.repeat(65));
  console.log(`Allowed Locations : ${options.allowedLocations.join(', ')}`);
  console.log(`Mode              : ${options.dryRun ? 'DRY-RUN (Preview only)' : 'LIVE (Auto-terminate unauthorized)'}`);
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
    // 1. Connect over CDP or Launch Persistent Context
    const cdpPort = options.cdpPort || (await isPortOpen(9222) ? '9222' : null);

    if (cdpPort) {
      const cdpUrl = `http://127.0.0.1:${cdpPort}`;
      console.log(`\nConnecting to existing browser over CDP (${cdpUrl})...`);
      browser = await chromium.connectOverCDP(cdpUrl);
      const contexts = browser.contexts();
      context = contexts.length > 0 ? contexts[0] : await browser.newContext();

      const pages = context.pages();
      page = pages.find(p => p.url().includes('claude.ai'));
      if (page) {
        console.log(`Found existing Claude tab: ${page.url()}`);
        await page.bringToFront();
      } else {
        page = await context.newPage();
      }
      isCdp = true;
    } else {
      if (!fs.existsSync(options.userDataDir)) {
        fs.mkdirSync(options.userDataDir, { recursive: true });
      }

      console.log(`\nLaunching desktop browser with persistent profile...`);
      const launchOptions = {
        headless: options.headless,
        viewport: { width: 1280, height: 850 },
        args: [
          '--disable-blink-features=AutomationControlled',
          '--no-default-browser-check'
        ]
      };

      try {
        context = await chromium.launchPersistentContext(options.userDataDir, {
          ...launchOptions,
          channel: 'chrome'
        });
      } catch (err) {
        console.log('Google Chrome channel not found, launching default Chromium...');
        context = await chromium.launchPersistentContext(options.userDataDir, launchOptions);
      }

      page = context.pages().length > 0 ? context.pages()[0] : await context.newPage();
    }

    // 2. Initial Authentication Check
    const settingsUrl = 'https://claude.ai/new#settings/account';
    console.log(`Navigating to ${settingsUrl}...`);
    await page.goto(settingsUrl, { waitUntil: 'domcontentloaded', timeout: 45000 });

    console.log('Checking authentication status...');
    let loggedIn = false;
    const maxWaitTime = 120000; // 2 minutes
    const startTime = Date.now();

    while (Date.now() - startTime < maxWaitTime && !isShuttingDown) {
      const currentUrl = page.url();
      const isLoginPage = currentUrl.includes('/login');
      const isTurnstile = (await page.title().catch(() => '')).includes('Just a moment');

      if (isLoginPage || isTurnstile) {
        console.log('\n[Action Required] Claude is prompting for login or verification.');
        console.log('Please log in to your Claude account in the opened browser window.');
        console.log('Waiting for login to complete (session will be saved for future runs)...\n');
        
        await page.waitForURL(url => !url.toString().includes('/login') && !url.toString().includes('turnstile'), {
          timeout: maxWaitTime - (Date.now() - startTime)
        }).catch(() => {});
      }

      const hasActiveSessions = await page.locator('text=Active sessions').isVisible().catch(() => false);
      const hasAccount = await page.locator('text=Log out of all devices').isVisible().catch(() => false);
      const hasSettings = await page.locator('[aria-label="Settings"], button:has-text("Settings")').isVisible().catch(() => false);
      const hasUserMenu = await page.locator('[data-testid="user-menu-button"]').isVisible().catch(() => false);

      if (hasActiveSessions || hasAccount || hasSettings || hasUserMenu) {
        loggedIn = true;
        break;
      }

      await page.waitForTimeout(2000);
    }

    if (!loggedIn) {
      throw new Error('Authentication timed out. Please log in and restart the bot.');
    }

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

module.exports = { main, isLocationAllowed, performScan };
