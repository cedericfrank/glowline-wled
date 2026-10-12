// Tests for tools/sign-release.mjs's check of the version compiled into the image. Run: npm test
// Uses a throwaway key in a temp dir, never $LUMEN_OTA_KEY_DIR: it can't match the embedded public
// key, so an image that passes the version check stops at the signature self-check, unsigned.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { generateKeyPairSync } from 'node:crypto';
import { existsSync, mkdtempSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const signer = join(dirname(fileURLToPath(import.meta.url)), 'sign-release.mjs');
const dir = mkdtempSync(join(tmpdir(), 'sign-release-test-'));
const { privateKey } = generateKeyPairSync('ec', { namedCurve: 'prime256v1' });
writeFileSync(join(dir, 'throwaway.pem'), privateKey.export({ type: 'pkcs8', format: 'pem' }), { mode: 0o600 });

// A fake image: ESP magic byte, filler, then each marker as a C string.
function image(...versions) {
  const parts = [Buffer.from([0xe9]), Buffer.alloc(64, 0x55)];
  for (const v of versions) parts.push(Buffer.from(`GLOWLINE_FW_VERSION=${v}\0`, 'latin1'), Buffer.alloc(16, 0x55));
  const path = join(dir, `fw-${versions.join('+') || 'none'}.bin`);
  writeFileSync(path, Buffer.concat(parts));
  return path;
}

function sign(bin, version) {
  const out = join(dir, `manifest-${Math.random().toString(36).slice(2)}.json`);
  const r = spawnSync(process.execPath, [signer, '--bin', bin, '--version', version, '--keyFile', 'throwaway.pem', '--out', out],
    { env: { ...process.env, LUMEN_OTA_KEY_DIR: dir }, encoding: 'utf8' });
  return { status: r.status, stderr: r.stderr, wrote: existsSync(out) };
}

test('refuses a 1.9.0-dev1 build signed as 1.9.0', () => {
  const r = sign(image('1.9.0-dev1'), '1.9.0');
  assert.equal(r.status, 1);
  assert.match(r.stderr, /built as "1\.9\.0-dev1", but --version is "1\.9\.0"/);
  assert.equal(r.wrote, false);
});

test('refuses an image without the marker', () => {
  const r = sign(image(), '1.9.0');
  assert.equal(r.status, 1);
  assert.match(r.stderr, /no GLOWLINE_FW_VERSION= marker/);
  assert.equal(r.wrote, false);
});

test('refuses an image with the marker twice', () => {
  const r = sign(image('1.9.0', '1.9.0'), '1.9.0');
  assert.equal(r.status, 1);
  assert.match(r.stderr, /built as "1\.9\.0", "1\.9\.0"/);
  assert.equal(r.wrote, false);
});

test('a matching version passes the check (and stops at the throwaway key)', () => {
  const r = sign(image('1.9.0'), '1.9.0');
  assert.equal(r.status, 1);
  assert.doesNotMatch(r.stderr, /built as|marker/);
  assert.match(r.stderr, /does NOT verify against the "primary" public key/);
  assert.equal(r.wrote, false);
});
