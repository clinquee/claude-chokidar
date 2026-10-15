#!/usr/bin/env node
/**
 * Supervisor for the always-on deployment.
 *
 * Runs `npm run bot:1m:headless`, watches its output and exit code, and when
 * something goes wrong it emails NOTIFY_EMAIL (plus an optional ntfy.sh push)
 * and PAUSES the bot instead of restarting it forever:
 *
 *   - session key expired / rejected at startup     (index.js exit code 2)
 *   - logged out mid-run, e.g. "Log out of all devices" (exit code 3)
 *   - crash loop: 4 crashes within 30 minutes
 *   - hung: no scan progress for 10 minutes
 *   - scans keep failing: 5 in a row could not read the Active sessions table
 *
 * Auth failures get one retry first, so a single Cloudflare hiccup does not page you.
 *
 * While paused, the bot stays stopped and a reminder goes out every 24h. It
 * resumes by itself when session-key.txt or .env changes (i.e. after
 * `npm run session-key -- <new key>`), or on `npm run chokidar:resume`. Once
 * the bot signs in again you get a "back online" email.
 *
 * Usage:
 *   node notify-wrapper.js                 supervise the bot (what systemd runs)
 *   node notify-wrapper.js --test [--all]  send a test email + push (--all: to NOTIFY_ALSO too)
 *   node notify-wrapper.js --alert <text>  send one alert and exit (systemd OnFailure)
 *   node notify-wrapper.js --resume        clear a pause (same as npm run chokidar:resume)
 *
 * Config, from .env next to this file (or the environment):
 *   SMTP_USER, SMTP_PASS   Gmail address + app password used to send mail (required)
 *   NOTIFY_EMAIL           recipient of every email (default shrey00710@gmail.com)
 *   NOTIFY_ALSO            comma-separated extra recipients for alerts
 *                          (pause / still paused / alert / back online), not tests
 *   SMTP_HOST, SMTP_PORT   default smtp.gmail.com:465
 *   NTFY_TOPIC             optional ntfy.sh topic for phone pushes
 *   HEALTHCHECK_URL        optional healthchecks.io ping URL: pinged after every
 *                          completed scan, /fail on pause. healthchecks.io alerts
 *                          from outside if the pings stop (VM down, wrapper dead)
 */

const fs = require('fs');
const os = require('os');
const path = require('path');
const crypto = require('crypto');
const https = require('https');
const { spawn } = require('child_process');

const DIR = __dirname;
const STATE_FILE = path.join(DIR, '.chokidar-paused.json');
const KEY_FILES = [path.join(DIR, 'session-key.txt'), path.join(DIR, '.env')];
const BOT_SCRIPT = 'bot:1m:headless';

const AUTH_RETRIES = 1;                    // auth failures tolerated before pausing
const AUTH_RETRY_DELAY_MS = 60 * 1000;
const CRASH_WINDOW_MS = 30 * 60 * 1000;
const MAX_CRASHES = 4;                     // within CRASH_WINDOW_MS, then pause
const MAX_FAILED_SCANS = 5;                // consecutive unreadable scans, then restart
const STALL_MS = 10 * 60 * 1000;           // no progress for this long = hung
const STARTUP_STALL_MS = 5 * 60 * 1000;    // never reached "Authentication confirmed"
const REMINDER_MS = 24 * 60 * 60 * 1000;
const STOP_GRACE_MS = 20 * 1000;
const LOG_TAIL_LINES = 40;

// --- config -----------------------------------------------------------------

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

function config() {
  const env = { ...loadDotEnv(path.join(DIR, '.env')), ...process.env };
  return {
    smtpUser: env.SMTP_USER,
    smtpPass: (env.SMTP_PASS || '').replace(/\s+/g, ''), // Gmail shows app passwords in groups of 4
    smtpHost: env.SMTP_HOST || 'smtp.gmail.com',
    smtpPort: parseInt(env.SMTP_PORT || '465', 10),
    to: env.NOTIFY_EMAIL || 'shrey00710@gmail.com',
    also: (env.NOTIFY_ALSO || '').split(',').map(a => a.trim()).filter(Boolean),
    ntfyTopic: env.NTFY_TOPIC,
    healthcheckUrl: env.HEALTHCHECK_URL
  };
}

// --- logging ----------------------------------------------------------------

const logTail = [];

function redact(text) {
  return String(text).replace(/sk-ant-[A-Za-z0-9_-]+/g, 'sk-ant-[redacted]');
}

function log(msg) {
  const line = `[wrapper] ${msg}`;
  console.log(line);
  remember(line);
}

function remember(line) {
  logTail.push(redact(line));
  if (logTail.length > LOG_TAIL_LINES) logTail.shift();
}

// --- notifications ----------------------------------------------------------

function sendEmail(subject, body, recipients) {
  const cfg = config();
  if (!cfg.smtpUser || !cfg.smtpPass) {
    return Promise.reject(new Error('SMTP_USER / SMTP_PASS are not set in .env'));
  }
  const nodemailer = require('nodemailer');
  const transport = nodemailer.createTransport({
    host: cfg.smtpHost,
    port: cfg.smtpPort,
    secure: cfg.smtpPort === 465,
    auth: { user: cfg.smtpUser, pass: cfg.smtpPass }
  });
  return transport.sendMail({
    from: `Claude Chokidar <${cfg.smtpUser}>`,
    to: recipients.join(', '),
    subject: `[chokidar] ${subject}`,
    text: body
  });
}

// Dead-man's switch ping. suffix '' = success, '/fail' = failure.
function pingHealthcheck(suffix = '', message = '') {
  const { healthcheckUrl } = config();
  if (!healthcheckUrl) return Promise.resolve();
  return new Promise((resolve, reject) => {
    const req = https.request(healthcheckUrl.replace(/\/+$/, '') + suffix, { method: 'POST', timeout: 10000 }, res => {
      res.resume();
      res.statusCode < 300 ? resolve() : reject(new Error(`healthcheck HTTP ${res.statusCode}`));
    });
    req.on('error', reject);
    req.on('timeout', () => req.destroy(new Error('healthcheck timeout')));
    req.end(redact(message).slice(0, 10000));
  });
}

function sendPush(title, message) {
  const { ntfyTopic } = config();
  if (!ntfyTopic) return Promise.resolve();
  return new Promise((resolve, reject) => {
    const req = https.request({
      host: 'ntfy.sh',
      path: `/${encodeURIComponent(ntfyTopic)}`,
      method: 'POST',
      headers: { Title: `chokidar: ${title}`.replace(/[^\x20-\x7E]/g, ''), Priority: 'high' },
      timeout: 15000
    }, res => { res.resume(); res.statusCode < 300 ? resolve() : reject(new Error(`ntfy HTTP ${res.statusCode}`)); });
    req.on('error', reject);
    req.on('timeout', () => req.destroy(new Error('ntfy timeout')));
    req.end(message.slice(0, 3000));
  });
}

// Email is the channel that matters; keep retrying it for an hour if Gmail or
// the network is down at the moment of the incident.
async function notify(subject, details, { includeLog = true, alert = true } = {}) {
  const cfg = config();
  const recipients = [cfg.to, ...(alert ? cfg.also : [])];
  const body = [
    details,
    '',
    `Host: ${os.hostname()}  |  Time: ${new Date().toString()}`,
    '',
    'How to fix / resume:',
    '  ssh ubuntu@13.205.231.48',
    '  cd ~/claude-chokidar && npm run session-key -- "<new sk-ant-sid02-... value>"',
    '  (the bot resumes by itself within ~15s of the key file changing)',
    '  Other problems: npm run chokidar:resume   |   logs: journalctl -u claude-chokidar -f',
    ...(includeLog ? ['', `Last ${logTail.length} log lines:`, ...logTail] : [])
  ].join('\n');

  sendPush(subject, details).catch(err => log(`ntfy push failed: ${err.message}`));

  for (let attempt = 1; attempt <= 12; attempt++) {
    try {
      await sendEmail(subject, body, recipients);
      log(`Email sent to ${recipients.join(', ')}: ${subject}`);
      return true;
    } catch (err) {
      log(`Email attempt ${attempt} failed: ${err.message}`);
      if (attempt < 12) await sleep(5 * 60 * 1000);
    }
  }
  return false;
}

// --- pause state ------------------------------------------------------------

function keyHash() {
  const h = crypto.createHash('sha256');
  for (const f of KEY_FILES) {
    try { h.update(fs.readFileSync(f)); } catch (e) { h.update('missing:' + f); }
  }
  return h.digest('hex');
}

function readState() {
  try { return JSON.parse(fs.readFileSync(STATE_FILE, 'utf8')); } catch (e) { return null; }
}

function writeState(state) {
  fs.writeFileSync(STATE_FILE, JSON.stringify(state, null, 2));
}

function clearState() {
  try { fs.unlinkSync(STATE_FILE); } catch (e) {}
}

// --- supervisor -------------------------------------------------------------

const sleep = ms => new Promise(r => setTimeout(r, ms));

let child = null;
let stoppingChild = false;   // we killed it on purpose (restart / pause / shutdown)
let shuttingDown = false;
let paused = null;           // current pause state, or null
let authFailures = 0;
let crashes = [];            // timestamps
let failedScans = 0;
let healthy = false;         // reached "Authentication confirmed" this run
let startedAt = 0;
let lastProgress = 0;
let announceRecovery = false; // send "back online" on the next healthy start
let restartTimer = null;
let killReason = null;       // why we killed the child, if we did

function startBot() {
  if (child || paused || shuttingDown) return;
  restartTimer = null;
  healthy = false;
  failedScans = 0;
  killReason = null;
  stoppingChild = false;
  startedAt = lastProgress = Date.now();
  log(`Starting: npm run ${BOT_SCRIPT}`);

  child = spawn('npm', ['run', '--silent', BOT_SCRIPT], {
    cwd: DIR,
    detached: true, // own process group, so Chrome dies with it
    stdio: ['ignore', 'pipe', 'pipe'],
    env: process.env
  });

  for (const stream of [child.stdout, child.stderr]) {
    let buf = '';
    stream.on('data', chunk => {
      buf += chunk.toString();
      let nl;
      while ((nl = buf.indexOf('\n')) >= 0) {
        onLine(buf.slice(0, nl).replace(/\r$/, ''));
        buf = buf.slice(nl + 1);
      }
    });
  }

  child.on('exit', (code, signal) => onExit(code, signal));
}

function onLine(line) {
  console.log(line);
  remember(line);

  if (line.includes('Authentication confirmed!')) {
    healthy = true;
    authFailures = 0;
    lastProgress = Date.now();
    if (announceRecovery) {
      announceRecovery = false;
      notify('Back online', 'The bot signed in to claude.ai again and is watching your sessions.', { includeLog: false });
    }
  } else if (/Check #\d+|Browser recycled|Session recovered/.test(line)) {
    lastProgress = Date.now();
  } else if (line.includes('Check complete.')) {
    lastProgress = Date.now();
    failedScans = 0;
    pingHealthcheck('', line).catch(err => log(`Healthcheck ping failed: ${err.message}`));
  } else if (/Could not find the "Active sessions"|Could not identify the Active sessions table|unexpected columns|Scan error:/.test(line)) {
    lastProgress = Date.now();
    failedScans++;
    if (failedScans >= MAX_FAILED_SCANS) {
      killBot(`${failedScans} scans in a row could not read the Active sessions table`);
    }
  }
}

function killBot(reason) {
  if (!child || stoppingChild) return;
  log(`Stopping bot: ${reason}`);
  killReason = reason;
  stoppingChild = true;
  const pid = child.pid;
  try { process.kill(-pid, 'SIGTERM'); } catch (e) {}
  setTimeout(() => { try { process.kill(-pid, 'SIGKILL'); } catch (e) {} }, STOP_GRACE_MS).unref();
}

function onExit(code, signal) {
  const ranMs = Date.now() - startedAt;
  // Take down anything left in the group (Chrome renderers, etc.).
  try { process.kill(-child.pid, 'SIGKILL'); } catch (e) {}
  child = null;
  log(`Bot exited (code ${code}, signal ${signal}) after ${Math.round(ranMs / 1000)}s.`);

  if (shuttingDown || paused) return;

  if (code === 2 || code === 3) {
    authFailures++;
    const why = code === 2
      ? 'claude.ai rejected the session key at sign-in - it has most likely expired or been revoked.'
      : 'The bot was signed in, then claude.ai logged this session out (someone used "Log out of all devices", or the key was revoked/expired).';
    if (authFailures > AUTH_RETRIES) {
      pause(code === 2 ? 'Session key expired or rejected' : 'Logged out of claude.ai', why);
    } else {
      log(`Auth problem (${why}) - retrying once in ${AUTH_RETRY_DELAY_MS / 1000}s before alerting.`);
      scheduleStart(AUTH_RETRY_DELAY_MS);
    }
    return;
  }

  const now = Date.now();
  crashes = crashes.filter(t => now - t < CRASH_WINDOW_MS);
  crashes.push(now);
  const reason = killReason || `the bot process exited unexpectedly (code ${code}, signal ${signal})`;
  if (crashes.length >= MAX_CRASHES) {
    pause('Bot keeps failing - paused',
      `The bot stopped ${crashes.length} times in the last ${CRASH_WINDOW_MS / 60000} minutes. Latest: ${reason}.`);
    return;
  }
  const delay = 10000 * crashes.length;
  log(`Restarting in ${delay / 1000}s (${crashes.length}/${MAX_CRASHES} failures in window; latest: ${reason}).`);
  scheduleStart(delay);
}

function scheduleStart(ms) {
  if (restartTimer) clearTimeout(restartTimer);
  restartTimer = setTimeout(startBot, ms);
}

function pause(reason, details) {
  log(`PAUSED: ${reason}`);
  if (restartTimer) { clearTimeout(restartTimer); restartTimer = null; }
  if (child) killBot(`pausing (${reason})`);
  paused = { reason, details, since: new Date().toISOString(), keyHash: keyHash(), lastEmailAt: Date.now() };
  writeState(paused);
  pingHealthcheck('/fail', `PAUSED: ${reason}
${details}`).catch(err => log(`Healthcheck fail ping failed: ${err.message}`));
  notify(`PAUSED - ${reason}`, `${details}\n\nThe watchdog is PAUSED: your Claude sessions are NOT being protected until this is fixed.`);
}

function resume(why) {
  log(`Resuming: ${why}`);
  paused = null;
  clearState();
  authFailures = 0;
  crashes = [];
  announceRecovery = true;
  startBot();
}

// Runs every 15s: watchdog for hangs, and resume / reminder checks while paused.
function tick() {
  if (shuttingDown) return;

  if (paused) {
    if (!fs.existsSync(STATE_FILE)) return resume('pause was cleared (npm run chokidar:resume)');
    if (keyHash() !== paused.keyHash) return resume('session key file changed');
    if (Date.now() - paused.lastEmailAt > REMINDER_MS) {
      paused.lastEmailAt = Date.now();
      writeState(paused);
      notify(`Still paused - ${paused.reason}`, `${paused.details}\n\nPaused since ${paused.since}. Sessions are still NOT being protected.`);
    }
    return;
  }

  if (child && !stoppingChild) {
    const now = Date.now();
    if (!healthy && now - startedAt > STARTUP_STALL_MS) {
      killBot(`no successful sign-in within ${STARTUP_STALL_MS / 60000} minutes of starting`);
    } else if (healthy && now - lastProgress > STALL_MS) {
      killBot(`hung - no scan activity for ${Math.round((now - lastProgress) / 60000)} minutes`);
    }
  }
}

function shutdown(sig) {
  if (shuttingDown) return;
  shuttingDown = true;
  log(`Received ${sig}, stopping.`);
  if (restartTimer) clearTimeout(restartTimer);
  if (!child) process.exit(0);
  killBot('wrapper shutting down');
  child.on('exit', () => process.exit(0));
}

async function main() {
  const args = process.argv.slice(2);

  if (args[0] === '--test') {
    const ok = await notify('Test notification',
      'This is a test from claude-chokidar. If you can read this, expiry/pause alerts will reach you.',
      { includeLog: false, alert: args[1] === '--all' });
    process.exit(ok ? 0 : 1);
  }
  if (args[0] === '--alert') {
    await pingHealthcheck('/fail', args.slice(1).join(' ')).catch(() => {});
    const ok = await notify('ALERT', args.slice(1).join(' ') || 'claude-chokidar needs attention.', { includeLog: false });
    process.exit(ok ? 0 : 1);
  }
  if (args[0] === '--resume') {
    clearState();
    console.log('Pause cleared. A running wrapper resumes the bot within ~15 seconds.');
    return;
  }

  const cfg = config();
  if (!cfg.smtpUser || !cfg.smtpPass) {
    log('WARNING: SMTP_USER / SMTP_PASS are not set - alerts cannot be emailed.');
  }

  process.on('SIGTERM', () => shutdown('SIGTERM'));
  process.on('SIGINT', () => shutdown('SIGINT'));
  setInterval(tick, 15000);

  const saved = readState();
  if (saved && saved.keyHash === keyHash()) {
    paused = saved;
    log(`Still paused from ${saved.since}: ${saved.reason}. Waiting for a new session key or npm run chokidar:resume.`);
    return;
  }
  if (saved) {
    clearState();
    announceRecovery = true;
    log('Session key changed while paused - resuming.');
  }
  startBot();
}

main();
