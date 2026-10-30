// SPDX-License-Identifier: EUPL-1.2
// Copyright (c) 2026 the Lumen authors. Part of a modified version of WLED; see NOTICE.md.
// Tests for tools/flash-manifest.mjs. Run: npm test
// Fake release folders in a temp dir: no real image, no key.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { existsSync, mkdirSync, mkdtempSync, readFileSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const tool = join(dirname(fileURLToPath(import.meta.url)), 'flash-manifest.mjs');
const root = mkdtempSync(join(tmpdir(), 'flash-manifest-test-'));
const sha256 = (buf) => createHash('sha256').update(buf).digest('hex');

// The Lumen partition table (tools/WLED_ESP32_16MB_9MB_FS.csv), as gen_esp32part writes it.
const LAYOUT = [
  ['nvs', 1, 2, 0x9000, 0x5000], ['otadata', 1, 0, 0xe000, 0x2000],
  ['app0', 0, 0x10, 0x10000, 0x300000], ['app1', 0, 0x11, 0x310000, 0x300000],
  ['spiffs', 1, 0x82, 0x610000, 0x9e0000], ['coredump', 1, 3, 0xff0000, 0x10000],
];
function partitionTable(layout = LAYOUT) {
  const buf = Buffer.alloc(0xc00, 0xff);
  layout.forEach(([label, type, subtype, offset, size], i) => {
    const e = buf.subarray(i * 32, i * 32 + 32);
    e.fill(0);
    e[0] = 0xaa; e[1] = 0x50; e[2] = type; e[3] = subtype;
    e.writeUInt32LE(offset, 4); e.writeUInt32LE(size, 8);
    e.write(label, 12, 'latin1');
  });
  return buf;
}

// A release folder; opts change one thing to make it wrong.
function release(name, opts = {}) {
  const dir = join(root, Math.random().toString(36).slice(2), name);
  mkdirSync(dir, { recursive: true });
  const host = opts.host ?? (name.includes('_prod') ? 'devices.buylumen.com' : 'glowline-app-dev.cedericfrank.workers.dev');
  const app = Buffer.concat([Buffer.from([0xe9]), Buffer.alloc(100, 0x55), Buffer.from(`${host}\0`, 'latin1')]);
  writeFileSync(join(dir, 'firmware.bin'), app);
  writeFileSync(join(dir, 'bootloader.bin'), Buffer.concat([Buffer.from([0xe9]), Buffer.alloc(200, 0x11)]));
  writeFileSync(join(dir, 'partitions.bin'), opts.table ?? partitionTable());
  writeFileSync(join(dir, 'boot_app0.bin'), Buffer.alloc(0x2000, 0));
  const version = name.split('-').slice(1).join('-');
  writeFileSync(join(dir, 'manifest.json'), JSON.stringify({
    version: opts.manifestVersion ?? version, size: app.length,
    sha256: opts.sha ?? sha256(app), keyId: 'primary', signature: opts.unsigned ? '' : 'MEUCIQ==',
  }));
  return dir;
}

function run(dir) {
  const r = spawnSync(process.execPath, [tool, '--dir', dir], { encoding: 'utf8' });
  return { status: r.status, stderr: r.stderr, wrote: existsSync(join(dir, 'flash.json')) };
}

test('writes the four parts with offsets, sizes and SHA-256', () => {
  const dir = release('esp32s3_lumen_dev-1.14.0-dev1');
  const r = run(dir);
  assert.equal(r.status, 0, r.stderr);
  const out = JSON.parse(readFileSync(join(dir, 'flash.json'), 'utf8'));
  assert.equal(out.format, 1);
  assert.equal(out.version, '1.14.0-dev1');
  assert.equal(out.env, 'dev');
  assert.deepEqual(out.parts.map((p) => [p.file, p.offset]),
    [['bootloader.bin', 0x0], ['partitions.bin', 0x8000], ['boot_app0.bin', 0xe000], ['firmware.bin', 0x10000]]);
  for (const p of out.parts) {
    const buf = readFileSync(join(dir, p.file));
    assert.equal(p.size, buf.length);
    assert.equal(p.sha256, sha256(buf));
  }
});

test('refuses a bench folder', () => {
  const r = run(release('esp32s3_lumen_dev_BENCH_ABORT30_DO_NOT_SHIP-1.9.0-abort30'));
  assert.equal(r.status, 1);
  assert.match(r.stderr, /bench test image/);
  assert.equal(r.wrote, false);
});

test('refuses a firmware.bin that is not the signed one', () => {
  const r = run(release('esp32s3_lumen_prod-1.14.0', { sha: '0'.repeat(64) }));
  assert.equal(r.status, 1);
  assert.match(r.stderr, /not the signed image/);
  assert.equal(r.wrote, false);
});

test('refuses an unsigned manifest', () => {
  const r = run(release('esp32s3_lumen_prod-1.14.0', { unsigned: true }));
  assert.equal(r.status, 1);
  assert.match(r.stderr, /no signature/);
  assert.equal(r.wrote, false);
});

test('refuses a manifest for another version', () => {
  const r = run(release('esp32s3_lumen_prod-1.14.0', { manifestVersion: '1.13.0' }));
  assert.equal(r.status, 1);
  assert.match(r.stderr, /version "1\.13\.0", the folder is 1\.14\.0/);
  assert.equal(r.wrote, false);
});

test('refuses a prod folder holding a dev image', () => {
  const r = run(release('esp32s3_lumen_prod-1.14.0', { host: 'glowline-app-dev.cedericfrank.workers.dev' }));
  assert.equal(r.status, 1);
  assert.match(r.stderr, /does not contain the prod host/);
  assert.equal(r.wrote, false);
});

test('refuses a partition table that is not the Lumen layout', () => {
  const moved = LAYOUT.map((e) => (e[0] === 'spiffs' ? ['spiffs', 1, 0x82, 0x400000, 0x9e0000] : e));
  const r = run(release('esp32s3_lumen_prod-1.14.0', { table: partitionTable(moved) }));
  assert.equal(r.status, 1);
  assert.match(r.stderr, /entry 5 is not spiffs at 0x610000/);
  assert.equal(r.wrote, false);
});
