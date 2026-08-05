import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import test from 'node:test';

const root = path.resolve(import.meta.dirname, '../..');
const read = (relative) => fs.readFileSync(path.join(root, relative), 'utf8');

test('TA-B2 changes the live actor generation instead of forging a snapshot', () => {
  const scenario = read('e2e/ToActorMessaging/Client/Scenarios/ta-b2-stale-ref-scenario.ts');
  const actor = read('e2e/ToActorMessaging/Server/Actor/main.ts');
  assert.doesNotMatch(scenario, /generation:\s*\(BigInt/);
  assert.match(scenario, /actors\/ta-b2\/destroy/);
  assert.match(scenario, /replacement\.actor\.objectGeneration/);
  assert.match(actor, /path: '\/actors\/ta-b2\/destroy'/);
});

test('TA-B3 removes and restores the real owner route while preserving the actor ref', () => {
  const scenario = read('e2e/ToActorMessaging/Client/Scenarios/ta-b3-route-disconnected-scenario.ts');
  const runner = read('e2e/ToActorMessaging/run_e2e.sh');
  assert.doesNotMatch(scenario, /nodeRid:\s*['"]to-actor-missing-route/);
  assert.match(scenario, /waitForControl\(options, 'route-disconnected'\)/);
  assert.match(scenario, /waitForControl\(options, 'route-restored'\)/);
  assert.match(scenario, /waitForRouteState\(options, actor\.actor, 'disconnected'\)/);
  assert.match(scenario, /waitForRouteState\(options, actor\.actor, 'connected'\)/);
  assert.match(runner, /ss -K dst 127\.0\.0\.1 dport = "\$ACTOR_ROUTER_PORT"/);
  assert.match(runner, /kill -CONT "\$ACTOR_PID"/);
});
