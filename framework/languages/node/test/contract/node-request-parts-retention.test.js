'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const { spawnSync } = require('node:child_process');

const GC_FLAG = '--expose-gc';
const framework = require('../../packages/framework/dist/internal');

// Spec 08 E1: the encoded request is written once for the binding submit.
// A request whose reply is still pending must not keep those outbound bytes
// reachable from its reply reactions; only the submit operation uses them.
function fixture(surface) {
  const submitted = [];
  const node = {
    requestToNode(_target, parts) {
      submitted.push(new WeakRef(parts));
      return { high: 1n, low: 1n };
    },
    requestToChannel(_channel, parts) {
      submitted.push(new WeakRef(parts));
      return { high: 1n, low: 1n };
    }
  };
  // The reply never arrives during the test: the request stays pending.
  const table = { submit(operation) { operation(); return new Promise(() => {}); } };
  const transport = new framework.ZLinkRuntimeRouteTransport(
    () => undefined, undefined,
    () => ({ meshNode: () => node, meshCompletionTable: () => table }),
    undefined, undefined, undefined, undefined, () => false
  );
  const request = () => surface === 'node'
    ? transport.request('mesh', 'peer', 'Packet', { body: 'x'.repeat(1024) }, 1000)
    : transport.requestToChannel('mesh', 'channel', 'Packet', { body: 'x'.repeat(1024) }, 1000);
  return { submitted, request };
}

for (const surface of ['node', 'channel']) {
  test(`pending ${surface} request does not retain its encoded parts`, async t => {
    if (runWithGc(t)) return;
    const f = fixture(surface);
    const pending = [];
    for (let index = 0; index < 8; index++) pending.push(f.request());
    assert.equal(f.submitted.length, 8);
    await new Promise(resolve => setImmediate(resolve));
    global.gc();
    const retained = f.submitted.filter(reference => reference.deref() !== undefined).length;
    t.diagnostic(`${retained} of 8 encoded part lists retained by pending requests`);
    assert.equal(retained, 0);
    // The pending results stay reachable for the whole check.
    assert.equal(pending.length, 8);
  });
}

function runWithGc(t) {
  if (typeof global.gc === 'function') return false;
  const result = spawnSync(process.execPath, [
    GC_FLAG, '--test', '--test-reporter=tap',
    `--test-name-pattern=^${t.name}$`, __filename
  ], { encoding: 'utf8', env: { ...process.env, NODE_TEST_CONTEXT: undefined } });
  assert.ifError(result.error);
  assert.equal(result.status, 0, `${result.stdout}\n${result.stderr}`);
  assert.match(result.stdout, /^# pass 1$/m, result.stdout);
  assert.match(result.stdout, /^# fail 0$/m, result.stdout);
  for (const line of result.stdout.split('\n')) {
    if (line.includes('encoded part lists retained')) t.diagnostic(line.replace(/^\s*# /, ''));
  }
  return true;
}
