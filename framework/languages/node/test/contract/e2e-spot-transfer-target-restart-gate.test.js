const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const nodeRoot = path.resolve(__dirname, '../..');

function read(relative) {
  return fs.readFileSync(path.join(nodeRoot, relative), 'utf8');
}

test('ST-H2 maps a two-phase target process restart to durable Join recovery assertions', () => {
  const client = read('e2e/SpotActorTransfer/Client/main.ts');
  const scenario = read(
    'e2e/SpotActorTransfer/Client/Scenarios/st-h2-target-process-restart-scenario.ts'
  );
  const server = read('e2e/SpotActorTransfer/Server/ActorNode/main.ts');
  const runner = read('e2e/SpotActorTransfer/run_e2e.sh');
  const featureMap = read('e2e/SpotActorTransfer/feature-map.ko.md');

  assert.match(client, /'ST-H2-PREPARE': prepareStH2TargetRestart/);
  assert.match(client, /'ST-H2-VERIFY': verifyStH2TargetRestart/);
  assert.match(runner, /elif \[\[ "\$SCENARIO" == "ST-H2" \]\]/);
  assert.match(runner, /crash_and_restart_actor_node "\$ST_H2_TARGET_NODE_RID"/);
  assert.match(runner, /kill -KILL "\$pid"/);
  assert.match(
    runner,
    /start_node actor-b "\$NODE_B_URL" "\$NODE_B_ROUTER" "\$NODE_B_PUBSUB"/
  );
  assert.match(runner, /wait_topology/);
  assert.match(runner, /run_client "ST-H2-VERIFY"/);

  assert.match(server, /factory\.executionMode\(ZLinkUserSpotExecutionMode\.PerActor\)/);
  assert.match(server, /factory\.recreateOnRelocation\(\)/);
  assert.match(server, /'join_completion_started'/);
  assert.match(server, /await completionGates\.wait\(this\.actorId\)/);
  assert.match(
    scenario,
    /createSpot\(\s*nodeB,\s*`h2-spot-\$\{fixtureId\}-\$\{attempt\}`/
  );
  assert.doesNotMatch(
    scenario,
    /createSpot\([^)]*targetNodeRid/,
    'ST-H2 must not request direct placement by NodeRid.'
  );
  assert.match(scenario, /authority\.objectGeneration === objectGeneration/);
  assert.match(scenario, /completions\.length === 1/);
  assert.match(scenario, /probe\.stateVersion === 121/);
  assert.match(scenario, /join_completion\|accepted\|\$\{operationId\}/);
  assert.match(scenario, /entry\.kind === 'packet_handler'/);
  assert.match(scenario, /entry\.value === 'after-target-restart'/);
  assert.match(featureMap, /\| `ST-H2` \| 구현 \|/);
});
