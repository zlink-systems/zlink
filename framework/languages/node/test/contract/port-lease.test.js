const assert = require('node:assert/strict');
const childProcess = require('node:child_process');
const crypto = require('node:crypto');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const test = require('node:test');

// samples/port-lease.mjs is the one lease protocol of the sample runners and
// the tests. Each case runs a second acquirer inside the first one's file
// operation, which is the interleaving two processes on the host can produce.

function leaseDirectory(t) {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'zlink-port-lease-test-'));
  t.after(() => fs.rmSync(directory, { recursive: true, force: true }));
  return directory;
}

function runInside(t, method, matches, interleaved) {
  const original = fs[method];
  let pending = true;
  fs[method] = function (...args) {
    if (pending && matches(args)) {
      pending = false;
      if (method === 'readFileSync') {
        const content = original.apply(this, args);
        interleaved();
        return content;
      }
      interleaved();
    }
    return original.apply(this, args);
  };
  t.after(() => {
    fs[method] = original;
  });
}

test('a lock being created is never taken as stale by a concurrent acquirer', async (t) => {
  const { acquirePortLease } = await import('../../samples/port-lease.mjs');
  const directory = leaseDirectory(t);
  let second;
  runInside(
    t,
    'writeFileSync',
    () => true,
    () => {
      second = acquirePortLease(28100, directory);
    }
  );
  const first = acquirePortLease(28100, directory);
  assert.equal([first, second].filter((lease) => lease !== undefined).length, 1);
  assert.ok(fs.readFileSync(path.join(directory, '28100.lock'), 'utf8').length > 0);
});

test('a late reclaimer of a stale lock does not remove the lock that replaced it', async (t) => {
  const { acquirePortLease } = await import('../../samples/port-lease.mjs');
  const directory = leaseDirectory(t);
  const deadPid = childProcess.spawnSync(process.execPath, ['-e', '']).pid;
  const leasePath = path.join(directory, '28101.lock');
  fs.writeFileSync(leasePath, `${deadPid} ${crypto.randomUUID()}\n`);
  let second;
  runInside(
    t,
    'readFileSync',
    ([file]) => file === leasePath,
    () => {
      second = acquirePortLease(28101, directory);
    }
  );
  const first = acquirePortLease(28101, directory);
  assert.equal(second, leasePath);
  assert.equal(first, undefined);
  assert.ok(fs.existsSync(leasePath));
  assert.deepEqual(fs.readdirSync(directory), ['28101.lock']);
});
