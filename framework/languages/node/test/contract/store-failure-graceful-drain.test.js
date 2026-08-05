'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const framework = require('../../packages/framework/dist/internal');

const workspaceRoot = path.resolve(__dirname, '../..');

test('drain keeps the marker observable for one configured polling interval', async () => {
  const pollingIntervalMs = 30;
  const host = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration({
      spotNodes: [{ name: 'game', router: { bind: 'tcp://127.0.0.1:1' } }],
      locations: { options: { pollingIntervalMs } }
    })
  });
  let markerPublishedAt;
  let cleanupStartedAt;
  host.locationOwner.runtime = {
    async publishDraining() {
      markerPublishedAt = performance.now();
      return true;
    },
    async cleanupOwner() {
      cleanupStartedAt = performance.now();
    }
  };
  host.serviceRelocation.relocateMesh = async () => {};
  host.stop = async () => {};

  assert.deepEqual(await host.routeMeshRuntime.drain('game', 500), { kind: 'drained' });
  assert.notEqual(markerPublishedAt, undefined);
  assert.notEqual(cleanupStartedAt, undefined);
  assert(
    cleanupStartedAt - markerPublishedAt >= pollingIntervalMs - 5,
    'owner cleanup started before peers had one polling interval to observe Draining=true'
  );
});

test('SF-C2 uses host relocation and verifies marker, terminal result, and clean exit', () => {
  const provider = read('e2e/DiscoveryRegistryHa/Server/Provider/Endpoints/provider-endpoints.ts');
  const providerHost = read('e2e/DiscoveryRegistryHa/Server/Provider/provider-host-factory.ts');
  const providerMain = read('e2e/DiscoveryRegistryHa/Server/Provider/main.ts');
  const scenario = read('e2e/DiscoveryRegistryHa/Client/Scenarios/SfC2GracefulShutdownScenario.ts');
  const runner = read('e2e/DiscoveryRegistryHa/run_e2e.sh');

  assert.match(provider, /configureServerSocket\(\)\.weight\s*=\s*0/);
  assert.match(provider, /frameworkRuntime\.relocate\(/);
  assert.match(scenario, /draining/);
  assert.match(scenario, /ZLinkFrameworkRelocationOutcome\.Relocated/);
  assert.match(scenario, /ZLinkFrameworkRelocationReason\.None/);
  assert.match(scenario, /providerRid\s*===\s*'api-a'/);
  assert.match(runner, /wait\s+"\$API_B_PID"/);
  assert.doesNotMatch(runner, /run_sf_c2\(\)[\s\S]*?\/shutdown/);
  assert.match(providerHost, /disposeLocationStore/);
  assert.doesNotMatch(providerMain, /process\.exit\(/);
});

function read(relativePath) {
  return fs.readFileSync(path.join(workspaceRoot, relativePath), 'utf8');
}
