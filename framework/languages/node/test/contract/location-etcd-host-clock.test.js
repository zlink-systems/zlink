const assert = require('node:assert/strict');
const path = require('node:path');
const test = require('node:test');
const {
  Harness,
  child,
  until,
  logRoot
} = require('../../../../../scripts/etcd/conformance-harness.cjs');
const { packageProviderDist } = require('../../../../../scripts/etcd/build-node-provider.cjs');
const preload = path.join(__dirname, 'location-etcd-fixtures/date-control.cjs');

// Remove this Node-only Date.now compatibility test when stage 3 provides isolated host-clock VM checks.
test(
  'Node provider and manager remain independent of Date.now jumps',
  {
    skip: process.env.ZLINK_ETCD_INTEGRATION !== '1',
    timeout: 180000
  },
  async (t) => {
    const dist = packageProviderDist();
    const adapter = path.join(__dirname, 'location-etcd-fixtures/provider-adapter.cjs');
    const h = new Harness(
      {
        node: {
          command: process.execPath,
          args: [adapter],
          env: { ZLINK_ETCD_PROVIDER_DIST: dist }
        }
      },
      path.dirname(dist)
    );
    try {
      await h.start();
      await t.test('etcd.provider-clock-independent (Node compatibility)', async () => {
        const f = await h.namespace('node-host-clock');
        for (const key of ['clock/a', 'clock/b'])
          await h.put(f, key, Buffer.from([0, 255, 1]), 60000);
        async function observations() {
          const results = [];
          for (const offset of [0, -86400000, 864000000]) {
            const task = child(
              process.execPath,
              [
                '--require',
                preload,
                path.join(__dirname, 'location-etcd-fixtures/provider-clock-process.cjs')
              ],
              {
                env: {
                  ...process.env,
                  ZLINK_ETCD_PROVIDER_DIST: dist,
                  ZLINK_ETCD_WALL_OFFSET: String(offset)
                },
                stdio: ['ignore', 'pipe', 'pipe', 'ipc']
              }
            );
            const message = new Promise((resolve, reject) => {
              task.running.once('message', resolve);
              task.exited.then(
                (r) =>
                  reject(new Error(`provider exited before observation: ${r.code}: ${r.output}`)),
                reject
              );
            });
            task.running.send({
              options: h.options(f.namespace),
              subject: 'clock/a',
              prefix: 'clock/'
            });
            const observed = await message;
            const exited = await task.exited;
            require('node:fs').writeFileSync(
              path.join(logRoot, `provider-clock-${f.namespace}-${offset}.log`),
              exited.output
            );
            assert.equal(exited.code, 0, JSON.stringify(observed) + exited.output);
            assert.equal(observed.error, undefined);
            results.push(observed.result);
          }
          assert.deepEqual(results[1], results[0]);
          assert.deepEqual(results[2], results[0]);
          return results[0];
        }
        const live = await observations();
        assert.equal(live.read.kind, 'found');
        assert.equal(live.write.kind, 'applied');
        assert.equal(live.scan.value.items.length, 1);
        assert.equal(live.next.value.items.length, 1);
        await h.setClock(f, BigInt(Date.parse(live.read.value.expiresAt)) + 1n);
        const expired = await observations();
        assert.equal(expired.read.kind, 'missing');
        assert.equal(expired.write.kind, 'conflict');
        assert.equal(expired.scan.value.items.length, 0);
      });
      await h.endCase();
      await t.test('etcd.manager-wall-clock-jumps (Node compatibility)', async () => {
        const f = await h.namespace('node-manager-wall');
        const manager = child(
          process.execPath,
          ['--require', preload, path.join(dist, 'admin.js'), 'run', '--config', f.file],
          {
            stdio: ['ignore', 'pipe', 'pipe', 'ipc']
          }
        );
        manager.label = 'node-wall-clock';
        h.managers.push(manager);
        const events = [];
        manager.running.on('message', (event) => events.push(event));
        let before = await h.clock(f);
        for (const offset of [-864000000, 864000000, 0]) {
          const started = performance.now();
          manager.running.send({ kind: 'wall', offset });
          await until(
            () => events.some((e) => e.kind === 'wall-set' && e.offset === offset),
            'wall adjustment acknowledged'
          );
          const after = await until(async () => {
            const c = await h.clock(f);
            return c.revision !== before.revision && c;
          }, 'clock after wall jump');
          assert.ok(after.value >= before.value);
          assert.ok(
            Number(after.value - before.value) <=
              1.01 * 1.0005 * (performance.now() - started) + 1010
          );
          before = after;
        }
        await h.stopManager(manager);
      });
    } finally {
      await h.close();
    }
  }
);
