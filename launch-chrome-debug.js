#!/usr/bin/env node

/**
 * Starts YOUR locally installed Chrome, on YOUR profile, with remote debugging
 * enabled, so the bot can attach to it with `node index.js --cdp 9222` instead
 * of signing in again.
 *
 * Chrome must be fully closed first: a profile can only be opened by one Chrome
 * process at a time, and debugging cannot be switched on after the fact.
 */

const http = require('http');
const { spawn } = require('child_process');
const chromeProfile = require('./chromeProfile');

const PORT = Number(process.env.CHROME_DEBUG_PORT || 9222);
const PROFILE_HINT = process.argv[2] || 'shreyans.tatiya@gmail.com';

function isPortOpen(port) {
  return new Promise(resolve => {
    const req = http.get(`http://127.0.0.1:${port}/json/version`, { timeout: 1000 }, res => {
      resolve(res.statusCode === 200);
    });
    req.on('error', () => resolve(false));
    req.on('timeout', () => { req.destroy(); resolve(false); });
  });
}

const sleep = ms => new Promise(r => setTimeout(r, ms));

(async () => {
  if (await isPortOpen(PORT)) {
    console.log(`Chrome is already listening for debugging on port ${PORT}.`);
    console.log(`Run:  node index.js --cdp ${PORT}`);
    return;
  }

  const chromePath = chromeProfile.findChromeExecutable();
  if (!chromePath) {
    console.error('Could not find chrome.exe. Set CHROME_PATH to its full path and retry.');
    process.exit(1);
  }

  const userDataDir = chromeProfile.defaultUserDataDir();
  const profile = chromeProfile.resolveProfile(userDataDir, PROFILE_HINT);
  if (!profile) {
    const { profiles } = chromeProfile.listProfiles(userDataDir);
    console.error(`No Chrome profile matched "${PROFILE_HINT}". Available:`);
    profiles.forEach(p => console.error(`  ${p.dir}  -  ${p.name}  ${p.email}`));
    process.exit(1);
  }

  if (chromeProfile.isChromeRunning()) {
    console.error('Chrome is currently running. Close every Chrome window first, then re-run this.');
    console.error('(Debugging cannot be enabled on a Chrome that is already up.)');
    process.exit(1);
  }

  console.log(`Chrome     : ${chromePath}`);
  console.log(`Profile    : ${profile.dir} (${profile.email || profile.name})`);
  console.log(`Debug port : ${PORT}`);

  const child = spawn(chromePath, [
    `--remote-debugging-port=${PORT}`,
    `--user-data-dir=${userDataDir}`,
    `--profile-directory=${profile.dir}`,
    '--no-first-run',
    '--no-default-browser-check',
    'https://claude.ai/settings/account'
  ], { detached: true, stdio: 'ignore' });
  child.unref();

  const deadline = Date.now() + 20000;
  while (Date.now() < deadline) {
    if (await isPortOpen(PORT)) {
      console.log(`\nChrome is up with debugging on ${PORT}. Now run:`);
      console.log(`  node index.js --cdp ${PORT}`);
      return;
    }
    await sleep(500);
  }

  console.error(`\nChrome started but never opened the debug port.`);
  console.error('Chrome 136+ blocks remote debugging when running on the default profile directory.');
  console.error('Use the bot profile instead (sign in once, it is then remembered):');
  console.error('  node index.js');
})();
