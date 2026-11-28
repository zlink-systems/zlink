// SPDX-License-Identifier: MPL-2.0

import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { existsSync, mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { constants, tmpdir } from 'node:os';
import path from 'node:path';
import test from 'node:test';

const zlink = require('@zlink-systems/zlink');
const { ConfigResult } = zlink;
const longSize = 4096;
const cases = {
  'missing': { xsub: false, index: 1, filter: '', results: [ConfigResult.NotFound] },
  'empty': { xsub: true, index: 0, filter: '', results: [ConfigResult.Ok] },
  'long': { xsub: false, index: 0, filter: 'x'.repeat(longSize), results: [ConfigResult.BufferTooSmall, ConfigResult.Ok] },
  'removed-before-retry': { xsub: true, index: 0, filter: 'x'.repeat(longSize), results: [ConfigResult.BufferTooSmall, ConfigResult.NotFound] },
  'invalid-argument': { xsub: false, index: 0, filter: '', results: [ConfigResult.InvalidArgument] },
  'buffer-still-too-small': { xsub: true, index: 0, filter: 'x'.repeat(longSize), results: [ConfigResult.BufferTooSmall, ConfigResult.BufferTooSmall] }
};
type Scenario = keyof typeof cases;

function probe(scenario: Scenario): void {
  const { xsub, filter, index, results } = cases[scenario];
  const context = zlink.createContext();
  const socket = xsub ? zlink.createXSubSocket(context) : zlink.createSubSocket(context);
  try {
    socket.setSubscription(filter);
    const result = results.at(-1);
    if (result === ConfigResult.InvalidArgument || result === ConfigResult.BufferTooSmall) {
      const nativeErrno = result === ConfigResult.InvalidArgument
        ? constants.errno.EINVAL : constants.errno.ENOBUFS;
      assert.throws(() => socket.subscriptionAt(index), { name: 'ConfigError', result, nativeErrno });
    } else {
      assert.deepEqual(socket.subscriptionAt(index),
        result === ConfigResult.NotFound ? null : { filter, isPattern: false });
    }
  } finally {
    socket.close();
    context.close();
  }
}

const selected = process.env.ZLINK_NODE_SUBSCRIPTION_CASE;
if (selected !== undefined) {
  probe(selected as Scenario);
} else {
  test('subscriptionAt Core calls', { skip: process.platform !== 'linux' }, async (t) => {
    const directory = mkdtempSync(path.join(tmpdir(), 'zlink-node-subscription-'));
    try {
      const shim = path.join(directory, 'subscription_at_shim.so');
      const compiler = spawnSync('cc', ['-shared', '-fPIC', '-o', shim,
        '-I', path.resolve(__dirname, '../../../../core/include'),
        path.resolve(__dirname, '../../tests/native/subscription_at_shim.c'), '-ldl'],
      { encoding: 'utf8', timeout: 30000 });
      assert.ifError(compiler.error);
      assert.equal(compiler.status, 0, compiler.stderr);

      for (const [scenario, { index, results }] of Object.entries(cases)) {
        await t.test(scenario, () => {
          const trace = path.join(directory, scenario + '.trace');
          const child = spawnSync(process.execPath, [__filename], {
            env: {
              ...process.env,
              LD_PRELOAD: process.env.LD_PRELOAD ? `${shim}:${process.env.LD_PRELOAD}` : shim,
              ZLINK_NODE_SUBSCRIPTION_CASE: scenario,
              ZLINK_NODE_SUBSCRIPTION_TRACE: trace
            },
            encoding: 'utf8',
            timeout: 10000
          });
          assert.ifError(child.error);
          const calls = existsSync(trace) ? readFileSync(trace, 'utf8').trim().split('\n') : [];
          assert.equal(child.signal, null, child.stderr);
          assert.equal(child.status, 0, `${child.stderr}\nCore calls (index, capacity, result): ${calls}`);
          assert.deepEqual(calls, results.map((result, attempt) =>
            `${index} ${attempt === 0 ? 256 : longSize} ${result}`));
        });
      }
    } finally {
      rmSync(directory, { recursive: true, force: true });
    }
  });
}
