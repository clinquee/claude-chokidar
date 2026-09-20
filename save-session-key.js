#!/usr/bin/env node

/**
 * Saves your claude.ai session cookies to session-key.txt (gitignored), so the
 * bot reuses your existing Claude session instead of signing in again and
 * adding another device to the active-sessions list.
 *
 * claude.ai sets several of these side by side (sessionKey, sessionKeyV3 and
 * their *LC variants). Rather than guessing which one is authoritative, paste
 * whichever you have - all of them is best, and the easiest way to get all of
 * them at once is the whole Cookie header:
 *
 *   F12 > Network > click any claude.ai request > Request Headers >
 *   right-click "cookie" > Copy value
 *
 * Or per cookie: F12 > Application > Cookies > https://claude.ai > copy Value.
 *
 * Usage:
 *   node save-session-key.js sk-ant-sid01-....
 *   node save-session-key.js "sessionKey=sk-...; sessionKeyV3=sk-..."
 *   npm run session-key -- "<paste>"
 *   node save-session-key.js          (then paste when prompted)
 *
 * Writing the file this way avoids PowerShell's `>` redirection, which saves
 * UTF-16 and would corrupt the value.
 */

const fs = require('fs');
const path = require('path');
const readline = require('readline');
const { parseSessionCookies } = require('./index.js');

const OUT_FILE = path.join(__dirname, 'session-key.txt');

function save(raw) {
  const cookies = parseSessionCookies(raw);

  if (!cookies.length) {
    console.error('No claude.ai session cookie found in what you pasted. Nothing written.');
    console.error('Expected a value like sk-ant-sid01-..., or "sessionKey=sk-ant-sid01-...".');
    process.exit(1);
  }

  const body = [
    '# claude.ai session cookies - treat this file like a password.',
    '# Regenerate with: node save-session-key.js "<paste cookie value>"',
    ...cookies.map(c => `${c.name}=${c.value}`)
  ].join('\n') + '\n';

  fs.writeFileSync(OUT_FILE, body, { encoding: 'utf8' });
  try {
    fs.chmodSync(OUT_FILE, 0o600);
  } catch (e) {}

  console.log(`Saved ${cookies.length} cookie(s) to ${OUT_FILE}:`);
  for (const c of cookies) {
    console.log(`  ${c.name} (${c.value.length} chars)`);
  }
  if (!cookies.some(c => c.value.startsWith('sk-ant-sid'))) {
    console.log('\nNote: claude.ai session values normally start with "sk-ant-sid".');
    console.log('If sign-in fails, re-copy the cookie value.');
  }
  console.log('\nThis file is gitignored. Now run:  npm start');
}

const fromArgs = process.argv.slice(2).join(' ').trim();
if (fromArgs) {
  save(fromArgs);
} else {
  const rl = readline.createInterface({ input: process.stdin, output: process.stdout });
  rl.question('Paste your claude.ai session cookie(s): ', answer => {
    rl.close();
    save(answer);
  });
}
