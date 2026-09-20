'use strict';

/**
 * Local Chrome profile support.
 *
 * Lets the bot drive the user's real, installed chrome.exe using a copy of
 * their own Chrome profile (cookies, logins, claude.ai session) instead of a
 * blank browser that needs a fresh login.
 *
 * Why a copy and not the real profile directory?
 *   - Chrome 136+ refuses to enable remote debugging (which Playwright needs)
 *     when it is started on the *default* user-data-dir, so automating the
 *     live profile in place is not possible.
 *   - Chrome also single-instance-locks a user-data-dir, so the bot would
 *     fight with the Chrome window you are using day to day.
 * Cloning the profile keeps your logins while leaving your everyday Chrome
 * untouched and usable at the same time.
 */

const fs = require('fs');
const path = require('path');
const os = require('os');
const { execFileSync } = require('child_process');

// Files/dirs inside a profile that carry identity (cookies, logins, site state).
// Caches, history and media are deliberately skipped: they are big and useless here.
const PROFILE_FILES = [
  'Preferences',
  'Secure Preferences',
  'Login Data',
  'Login Data For Account',
  'Web Data',
  'Trust Tokens'
];

const PROFILE_DIRS = [
  'Network',        // Cookies live here
  'Local Storage',
  'Session Storage'
];

const TOP_LEVEL_FILES = [
  'Local State',    // holds the DPAPI-wrapped cookie encryption key
  'First Run'
];

function chromeCandidates() {
  if (process.platform === 'win32') {
    const roots = [
      process.env['PROGRAMFILES'],
      process.env['PROGRAMFILES(X86)'],
      process.env['LOCALAPPDATA']
    ].filter(Boolean);
    return roots.map(r => path.join(r, 'Google', 'Chrome', 'Application', 'chrome.exe'));
  }
  if (process.platform === 'darwin') {
    return ['/Applications/Google Chrome.app/Contents/MacOS/Google Chrome'];
  }
  return ['/usr/bin/google-chrome', '/usr/bin/google-chrome-stable', '/opt/google/chrome/chrome'];
}

// Locate the user's installed Chrome binary (not Playwright's bundled Chromium).
function findChromeExecutable(explicitPath) {
  const candidates = [];
  if (explicitPath) candidates.push(explicitPath);
  if (process.env.CHROME_PATH) candidates.push(process.env.CHROME_PATH);
  candidates.push(...chromeCandidates());

  for (const candidate of candidates) {
    if (candidate && fs.existsSync(candidate)) return candidate;
  }

  if (process.platform === 'win32') {
    // Fall back to the registry entry Chrome writes on install.
    try {
      const out = execFileSync('reg', [
        'query',
        'HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\chrome.exe',
        '/ve'
      ], { encoding: 'utf8', stdio: ['ignore', 'pipe', 'ignore'] });
      const match = out.match(/REG_SZ\s+(.+chrome\.exe)/i);
      if (match && fs.existsSync(match[1].trim())) return match[1].trim();
    } catch (e) {}
  }

  return null;
}

// Default location of the live Chrome user-data-dir for this OS user.
function defaultUserDataDir() {
  if (process.platform === 'win32') {
    return path.join(process.env.LOCALAPPDATA || '', 'Google', 'Chrome', 'User Data');
  }
  if (process.platform === 'darwin') {
    return path.join(os.homedir(), 'Library', 'Application Support', 'Google', 'Chrome');
  }
  return path.join(os.homedir(), '.config', 'google-chrome');
}

// Read the profile list Chrome keeps in "Local State".
function listProfiles(userDataDir) {
  const localState = path.join(userDataDir, 'Local State');
  const profiles = [];
  let lastUsed = null;

  try {
    const state = JSON.parse(fs.readFileSync(localState, 'utf8'));
    lastUsed = state.profile && state.profile.last_used;
    const cache = (state.profile && state.profile.info_cache) || {};
    for (const [dir, info] of Object.entries(cache)) {
      if (!fs.existsSync(path.join(userDataDir, dir))) continue;
      profiles.push({
        dir,
        name: info.name || dir,
        email: info.user_name || info.gaia_name || ''
      });
    }
  } catch (e) {
    // No Local State (or unreadable): fall back to directory names on disk.
    try {
      for (const entry of fs.readdirSync(userDataDir, { withFileTypes: true })) {
        if (!entry.isDirectory()) continue;
        if (entry.name === 'Default' || /^Profile \d+$/.test(entry.name)) {
          profiles.push({ dir: entry.name, name: entry.name, email: '' });
        }
      }
    } catch (e2) {}
  }

  return { profiles, lastUsed };
}

// Resolve a user-supplied hint ("Profile 1", "Person 1", an email, or a
// substring of any of those) to a concrete profile directory.
function resolveProfile(userDataDir, hint) {
  const { profiles, lastUsed } = listProfiles(userDataDir);
  if (profiles.length === 0) return null;

  if (hint) {
    const needle = hint.trim().toLowerCase();
    const exact = profiles.find(p =>
      p.dir.toLowerCase() === needle ||
      p.name.toLowerCase() === needle ||
      p.email.toLowerCase() === needle
    );
    if (exact) return exact;

    const partial = profiles.find(p =>
      p.dir.toLowerCase().includes(needle) ||
      p.name.toLowerCase().includes(needle) ||
      p.email.toLowerCase().includes(needle)
    );
    if (partial) return partial;
    return null;
  }

  const last = profiles.find(p => p.dir === lastUsed);
  return last || profiles[0];
}

// Is this profile currently open in a running Chrome? Its cookie DB is locked if so.
function isProfileLocked(userDataDir, profileDir) {
  const cookies = path.join(userDataDir, profileDir, 'Network', 'Cookies');
  if (!fs.existsSync(cookies)) return false;
  try {
    const fd = fs.openSync(cookies, 'r+');
    fs.closeSync(fd);
    return false;
  } catch (e) {
    return true;
  }
}

function copyFileIfPresent(src, dest) {
  if (!fs.existsSync(src)) return { copied: false, locked: false };
  try {
    fs.mkdirSync(path.dirname(dest), { recursive: true });
    fs.copyFileSync(src, dest);
    return { copied: true, locked: false };
  } catch (e) {
    return { copied: false, locked: e.code === 'EBUSY' || e.code === 'EPERM' || e.code === 'EACCES' };
  }
}

function copyDirIfPresent(src, dest) {
  if (!fs.existsSync(src)) return { copied: 0, locked: 0 };
  let copied = 0;
  let locked = 0;
  fs.mkdirSync(dest, { recursive: true });
  for (const entry of fs.readdirSync(src, { withFileTypes: true })) {
    const from = path.join(src, entry.name);
    const to = path.join(dest, entry.name);
    if (entry.isDirectory()) {
      const sub = copyDirIfPresent(from, to);
      copied += sub.copied;
      locked += sub.locked;
    } else if (entry.isFile()) {
      const res = copyFileIfPresent(from, to);
      if (res.copied) copied++;
      else if (res.locked) locked++;
    }
  }
  return { copied, locked };
}

/**
 * Copy the identity-carrying parts of a real Chrome profile into `destDir`,
 * laid out as a standalone user-data-dir whose "Default" profile is the clone.
 *
 * Returns { ok, locked, profile, destDir, refreshed }.
 */
function cloneProfile({ userDataDir, profileDir, destDir, refresh = true, log = console.log }) {
  const srcProfile = path.join(userDataDir, profileDir);
  const destProfile = path.join(destDir, 'Default');

  if (!fs.existsSync(srcProfile)) {
    return { ok: false, locked: false, reason: `Chrome profile not found: ${srcProfile}` };
  }

  const alreadyCloned = fs.existsSync(path.join(destProfile, 'Network', 'Cookies'));
  if (alreadyCloned && !refresh) {
    return { ok: true, locked: false, refreshed: false, destDir, destProfile };
  }

  const locked = isProfileLocked(userDataDir, profileDir);
  if (locked && alreadyCloned) {
    log(`  Chrome is using "${profileDir}" right now - keeping the previously copied login instead.`);
    return { ok: true, locked: true, refreshed: false, destDir, destProfile };
  }

  fs.mkdirSync(destProfile, { recursive: true });

  let lockedCount = 0;
  for (const file of TOP_LEVEL_FILES) {
    const res = copyFileIfPresent(path.join(userDataDir, file), path.join(destDir, file));
    if (res.locked) lockedCount++;
  }
  for (const file of PROFILE_FILES) {
    const res = copyFileIfPresent(path.join(srcProfile, file), path.join(destProfile, file));
    if (res.locked) lockedCount++;
  }
  for (const dir of PROFILE_DIRS) {
    const res = copyDirIfPresent(path.join(srcProfile, dir), path.join(destProfile, dir));
    lockedCount += res.locked;
  }

  const gotCookies = fs.existsSync(path.join(destProfile, 'Network', 'Cookies'));
  return {
    ok: gotCookies,
    locked: lockedCount > 0,
    refreshed: true,
    destDir,
    destProfile,
    reason: gotCookies ? null : 'Could not copy the cookie database (Chrome may be holding it open).'
  };
}

// Is any Chrome browser process currently running for this user?
function isChromeRunning() {
  try {
    if (process.platform === 'win32') {
      const out = execFileSync('tasklist', ['/FI', 'IMAGENAME eq chrome.exe', '/NH'], {
        encoding: 'utf8',
        stdio: ['ignore', 'pipe', 'ignore']
      });
      return /chrome\.exe/i.test(out);
    }
    const out = execFileSync('pgrep', ['-x', process.platform === 'darwin' ? 'Google Chrome' : 'chrome'], {
      encoding: 'utf8',
      stdio: ['ignore', 'pipe', 'ignore']
    });
    return out.trim().length > 0;
  } catch (e) {
    return false;
  }
}

module.exports = {
  isChromeRunning,
  findChromeExecutable,
  defaultUserDataDir,
  listProfiles,
  resolveProfile,
  isProfileLocked,
  cloneProfile
};
