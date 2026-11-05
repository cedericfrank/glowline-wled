// Builds and runs each host test of the glowline usermod's pure logic
// (usermods/glowline/test/*_test.cpp: burn-in REQ-092, boot cue REQ-140, Wi-Fi list and
// wrong-password restart REQ-135) with the host C++ compiler ($CXX, else c++).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { mkdtempSync, readdirSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const testDir = join(root, 'usermods/glowline/test');
const sources = readdirSync(testDir).filter((f) => f.endsWith('_test.cpp')).sort();

test('glowline host tests exist', () => {
  assert.deepEqual(sources, ['burnin_test.cpp', 'cue_test.cpp', 'scan_test.cpp', 'wrongpw_test.cpp']);
});

for (const src of sources) {
  test(`glowline ${src} (host C++ test)`, () => {
    const dir = mkdtempSync(join(tmpdir(), 'glowline-host-'));
    try {
      const bin = join(dir, src.replace(/\.cpp$/, ''));
      execFileSync(process.env.CXX || 'c++',
        ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-o', bin, join(testDir, src)],
        { stdio: 'inherit' });
      const out = execFileSync(bin, { encoding: 'utf8' });
      assert.match(out, /all checks passed/);
    } finally {
      rmSync(dir, { recursive: true, force: true });
    }
  });
}
