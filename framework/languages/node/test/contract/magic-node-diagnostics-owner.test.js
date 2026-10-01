'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const packageRoot = path.resolve(__dirname, '../../packages');
const source = (name) => fs.readFileSync(path.join(packageRoot, name), 'utf8');
const framework = 'framework/src/runtime/';

const structuralPaths = [
  'actors/actor-runtime-state.ts', 'execution/serial-execution-queue.ts', 'execution/state-lane.ts',
  'host/route-mesh-runtime.ts', 'host/service-relocation-host-runtime.ts', 'streams/session-context.ts'
];
const relocationPaths = [
  'diagnostics/index.ts', 'host/actor-transfer-runtime.ts', 'host/route-mesh-runtime.ts',
  'host/service-relocation-host-runtime.ts'
];

test('structural guard environment decision has one owner', () => {
  const combined = structuralPaths.map((name) => source(framework + name)).join('\n');
  assert.equal((combined.match(/ZLINK_NODE_STRUCTURAL_GUARD/g) ?? []).length, 1);
});

test('relocation debug environment decision has one owner', () => {
  const combined = relocationPaths.map((name) => source(framework + name)).join('\n');
  assert.equal((combined.match(/ZLINK_DEBUG_FRAMEWORK_RELOCATION/g) ?? []).length, 1);
});

test('message flow event identifiers and diagnostic length have one definition', () => {
  const flow = source(framework + 'diagnostics/message-flow.ts');
  for (const eventId of ['zlink.message_flow', 'zlink.dispatch_error']) {
    assert.equal(flow.split(eventId).length - 1, 1, eventId);
  }
  const combined = flow + source(framework + 'diagnostics/dispatch-error-details.ts');
  assert.equal((combined.match(/ERROR_MESSAGE_MAX_LENGTH\s*=\s*512/g) ?? []).length, 1);
});

test('FlowOrigin lowercase vocabulary has one owner', () => {
  const combined = ['actors/bound-session-wire.ts', 'diagnostics/message-flow.ts', 'diagnostics/flow-context.ts']
    .map((name) => source(framework + name)).join('\n');
  for (const value of ['inbound', 'timer', 'application', 'lifecycle']) {
    assert.equal((combined.match(new RegExp(`: ['"]${value}['"]`, 'g')) ?? []).length, 1, value);
  }
});

test('STREAM control packet vocabulary has one owner', () => {
  const combined = [
    'framework/src/runtime/streams/protocol.ts', 'stream-wire/src/index.ts',
    'stream-connector/src/Runtime/Protocol/ZlinkSessionClosing.ts',
    'stream-connector/src/Runtime/Protocol/ZlinkStreamFrameProtocol.ts',
    'stream-connector/src/Runtime/ZlinkStreamActors.ts'
  ].map(source).join('\n');
  for (const packet of ['$zlink.heartbeat.ping', '$zlink.heartbeat.pong', '$zlink.actor.bound', '$zlink.actor.unbound', 'session-closing']) {
    const escaped = packet.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
    assert.equal((combined.match(new RegExp(`['"]${escaped}['"]`, 'g')) ?? []).length, 1, packet);
  }
});

test('runtime metric name consumers share their existing catalog owner', () => {
  const combined = [
    'diagnostics/runtime-metrics.ts', 'actors/index.ts', 'actors/transferred-actor-rollback.ts',
    'channels/dispatch-error-reporter.ts', 'host/index.ts', 'host/service-relocation-host-runtime.ts',
    'spots/spot-lifecycle-metrics.ts', 'streams/stream-session-runtime.ts'
  ].map((name) => source(framework + name)).join('\n');
  for (const name of ['zlink.actor.count', 'zlink.spot.count', 'zlink.stream.connections.opened', 'zlink.host.relocation.blocked']) {
    assert.equal(combined.split(name).length - 1, 1, name);
  }
});

test('owner count regression rejects duplicate policy and vocabulary inputs', () => {
  assert.throws(() => assert.equal(('ZLINK_NODE_STRUCTURAL_GUARD ZLINK_NODE_STRUCTURAL_GUARD'.match(/ZLINK_NODE_STRUCTURAL_GUARD/g) ?? []).length, 1));
  assert.throws(() => assert.equal("'zlink.message_flow' 'zlink.message_flow'".split('zlink.message_flow').length - 1, 1));
});

function assertRelocationDetailGates(text) {
  const ts = require('typescript');
  const ast = ts.createSourceFile('debug-caller.ts', text, ts.ScriptTarget.Latest, true);
  let calls = 0;
  function visit(node) {
    if (ts.isCallExpression(node) && ts.isIdentifier(node.expression) && node.expression.text === 'relocationDebug') {
      calls += 1;
      let gated = false;
      for (let parent = node.parent; parent !== undefined; parent = parent.parent) {
        if (ts.isIfStatement(parent) && parent.expression.getText(ast) === 'isRelocationDebugEnabled()') {
          gated = true;
          break;
        }
      }
      assert.equal(gated, true, 'debug detail construction must be behind the existing enabled gate');
    }
    ts.forEachChild(node, visit);
  }
  visit(ast);
  assert.ok(calls > 0, 'expected relocation diagnostic calls');
}

test('relocation debug detail construction is gated and ungated counterexamples fail', () => {
  assertRelocationDetailGates(source(framework + 'host/actor-transfer-runtime.ts'));
  assertRelocationDetailGates(source(framework + 'host/service-relocation-host-runtime.ts'));
  assert.throws(() => assertRelocationDetailGates("relocationDebug('event', { id: expensiveId() });"));
});

test('JSON contract schema property and required key checks share their local decision owner', () => {
  const json = source('framework/src/contracts/Handlers/JsonContract.ts');
  assert.equal((json.match(/=== '__proto__'/g) ?? []).length, 1);
  assert.equal((json.match(/=== 'prototype'/g) ?? []).length, 1);
  assert.equal((json.match(/=== 'constructor'/g) ?? []).length, 1);
});
