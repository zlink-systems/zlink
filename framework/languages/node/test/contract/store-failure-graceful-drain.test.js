'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const framework = require('../../packages/framework/dist/internal');

const workspaceRoot = path.resolve(__dirname, '../..');

test('Shutdown awaits Draining publication then runs callbacks and cleanup without a polling wait', async (t) => {
  t.mock.timers.enable({ apis: ['setTimeout'] });
  const host = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration({
      spotNodes: [{ name: 'game', router: { bind: 'tcp://127.0.0.1:1' } }],
      locations: { options: { pollingIntervalMs: 1000 } }
    })
  });
  const events = [];
  let publish;
  host.locationOwner.runtime = {
    async publishDraining() {
      events.push('publish');
      return await new Promise(resolve => { publish = resolve; });
    },
    async cleanupOwner() { events.push('owner-cleanup'); }
  };
  host.spotManager = { async drainForShutdown() { events.push('spot-callbacks-and-cleanup'); } };
  host.stop = async () => { events.push('stop'); };

  const shutdown = host.shutdown({ deadlineMs: 500 });
  await new Promise(resolve => setImmediate(resolve));
  assert.deepEqual(events, ['publish']);
  publish(true);
  await new Promise(resolve => setImmediate(resolve));
  // No timeout has advanced: successful publication is enough to start cleanup (§14).
  assert.deepEqual(events, ['publish', 'spot-callbacks-and-cleanup', 'owner-cleanup', 'stop']);
  assert.equal((await shutdown).outcome, framework.ZLinkFrameworkTerminationOutcome.Stopped);
});

for (const failure of ['publication-false', 'publication-throw', 'owner-cleanup']) {
  test(`Shutdown consumes the first ${failure} terminal without a polling wait`, { timeout: 5_000 }, async (t) => {
    const pollingIntervalMs = 1_000;
    const host = new framework.ZLinkFrameworkRuntimeHost({
      registration: framework.createFrameworkRegistration({
        spotNodes: [{ name: 'game', router: { bind: 'tcp://127.0.0.1:1' } }],
        locations: { options: { pollingIntervalMs } }
      })
    });
    const cause = new Error(`${failure} store failure`);
    let publications = 0;
    let cleanups = 0;
    let stops = 0;
    let observedFailure;
    host.locationOwner.runtime = {
      async publishDraining() {
        publications++;
        if (failure === 'publication-throw') throw cause;
        return failure !== 'publication-false';
      },
      async cleanupOwner() {
        cleanups++;
        throw cause;
      }
    };
    host.spotManager = { async drainForShutdown() {} };
    host.stop = async () => { stops++; };
    const operation = failure === 'owner-cleanup' ? 'cleanupOwnerForDrain' : 'publishHostDraining';
    const original = host[operation].bind(host);
    t.mock.method(host, operation, async (...args) => {
      try {
        return await original(...args);
      } catch (error) {
        observedFailure = error;
        throw error;
      }
    });

    const startedAt = performance.now();
    const result = await host.shutdown({ deadlineMs: 2_500 });
    const elapsed = performance.now() - startedAt;
    assert.ok(elapsed < pollingIntervalMs, `first failure finished shutdown after ${elapsed} ms`);
    assert.equal(result.outcome, framework.ZLinkFrameworkTerminationOutcome.ForceStopped);
    assert.equal(result.reason, framework.ZLinkFrameworkTerminationReason.TeardownFailed);
    assert.equal(publications, 1);
    assert.equal(cleanups, failure === 'owner-cleanup' ? 1 : 0);
    assert.equal(stops, 1);
    assert.equal(observedFailure.name, failure === 'owner-cleanup'
      ? 'ZLinkOwnerCleanupError' : 'ZLinkDrainingStatePublishError');
    if (failure === 'publication-false') {
      assert.equal(observedFailure.cause.message, 'Draining descriptor publication returned false.');
    } else {
      assert.equal(observedFailure.cause, cause);
    }
  });
}

test('SF-C2 uses host relocation and verifies marker, terminal result, and clean exit', () => {
  const provider = read('e2e/DiscoveryRegistryHa/Server/Provider/Endpoints/provider-endpoints.ts');
  const providerHost = read('e2e/DiscoveryRegistryHa/Server/Provider/provider-host-factory.ts');
  const providerMain = read('e2e/DiscoveryRegistryHa/Server/Provider/main.ts');
  const scenario = read('e2e/DiscoveryRegistryHa/Client/Scenarios/SfC2GracefulShutdownScenario.ts');
  const runner = read('e2e/DiscoveryRegistryHa/run_e2e.sh');

  assert.match(provider, /runtimeOptions\.mesh\([^\n]+\)\.placementWeight\s*=\s*0/);
  assert.match(provider, /frameworkRuntime\.relocate\(/);
  assert.match(scenario, /draining/);
  assert.match(scenario, /result\.outcome\s*===\s*0\s*&&\s*result\.reason\s*===\s*0/);
  assert.match(scenario, /providerRid\s*===\s*'api-a'/);
  assert.match(runner, /wait\s+"\$API_B_PID"/);
  assert.doesNotMatch(runner, /run_sf_c2\(\)[\s\S]*?\/shutdown/);
  assert.match(providerHost, /disposeLocationStore/);
  assert.doesNotMatch(providerMain, /process\.exit\(/);
});

function read(relativePath) {
  return fs.readFileSync(path.join(workspaceRoot, relativePath), 'utf8');
}
