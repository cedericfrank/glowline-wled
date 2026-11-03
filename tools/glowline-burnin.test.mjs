// Builds and runs the host test of the glowline usermod's factory burn-in logic
// (usermods/glowline/glowline_burnin.h, REQ-092) with the host C++ compiler ($CXX, else c++).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');

test('glowline burn-in state machine (host C++ test)', () => {
  const dir = mkdtempSync(join(tmpdir(), 'glowline-burnin-'));
  try {
    const bin = join(dir, 'burnin_test');
    execFileSync(process.env.CXX || 'c++',
      ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-o', bin,
       join(root, 'usermods/glowline/test/burnin_test.cpp')],
      { stdio: 'inherit' });
    const out = execFileSync(bin, { encoding: 'utf8' });
    assert.match(out, /all checks passed/);
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});
