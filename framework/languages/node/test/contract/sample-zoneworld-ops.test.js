const assert = require('node:assert/strict');
const test = require('node:test');

const root = '../../samples/ZoneWorld/dist/';
const { NodeRuntimeState } = require(root + 'Server/ZoneNode/Domain/node-runtime-state');
const { OpsReportAdapter } = require(root + 'Server/ZoneNode/Infrastructure/ZLink/Monitoring/ops-report-adapter');
const { ApplyNodeMaintenanceHandler } = require(root + 'Server/ZoneNode/Infrastructure/ZLink/Handlers/node-channel-handlers');
const { ReportNodeStatusHandler } = require(root + 'Server/Ops/ops-handlers');
const { NodeRegistry } = require(root + 'Server/Ops/node-registry');
const { OpsConsoleRegistry } = require(root + 'Server/Ops/ops-console-registry');
const { ZoneWorldNames } = require(root + 'Shared/spec');

test('ZoneWorld maintenance reports current owner status to watching consoles before its reply, without a timer tick', async () => {
  const config = { zoneNode: { nodeId: 'zone-node-2' } };
  const state = new NodeRuntimeState(config);
  state.hostZone('zone-se');
  state.hostZone('zone-ne');
  state.joined('player-1', 'zone-ne');
  const nodes = new NodeRegistry();
  nodes.applyLiveRoutingIds(new Set(['owner-east', 'owner-west']));
  nodes.report({ nodeId: 'zone-node-1', zones: ['zone-nw'], playerCount: 0, maintenance: false }, 'owner-west');
  const consoles = new OpsConsoleRegistry();
  const pushes = [];
  consoles.add({
    sessionId: 'console',
    client: { send(message) { return { async submit() { pushes.push(message); } }; } }
  });
  const receiver = new ReportNodeStatusHandler(nodes, consoles);
  const reports = new OpsReportAdapter({
    sendToChannel(channel, message) {
      assert.equal(channel, ZoneWorldNames.reportChannel);
      return { submit: () => receiver.handle(message, { sourceNodeRid: 'owner-east' }) };
    }
  }, state);
  const handler = new ApplyNodeMaintenanceHandler(config, state, reports);

  for (const enabled of [true, false]) {
    const before = pushes.length;
    const reply = await handler.handle({ nodeId: state.nodeId, enabled }, {});
    assert.equal(pushes.length, before + 1, 'the applied transition must not wait for the periodic report');
    assert.deepEqual({ ...pushes.at(-1) }, {
      nodeId: state.nodeId, registered: true, connected: true, maintenance: enabled,
      zones: ['zone-ne', 'zone-se'], playerCount: 1
    });
    assert.deepEqual(reply, { nodeId: state.nodeId, enabled, zones: ['zone-ne', 'zone-se'] });
    assert.equal(nodes.snapshot().find(node => node.nodeId === 'zone-node-1').maintenance, false);
  }

  await assert.rejects(handler.handle({ nodeId: 'zone-node-1', enabled: true }, {}), /Maintenance request targets/);
  assert.equal(pushes.length, 2);
  assert.equal(state.ownMaintenance(), false);
});
