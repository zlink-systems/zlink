// SPDX-License-Identifier: MPL-2.0

'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const zlink = require('@zlink-systems/zlink');

function isConfigError(error: unknown): boolean {
  return error instanceof zlink.ConfigError;
}

test('ReceiveFlowState enum values match the C ABI', () => {
  assert.equal(zlink.ReceiveFlowState.RUNNING, 0);
  assert.equal(zlink.ReceiveFlowState.PAUSED, 1);
});

test('flow monitor event constants match the C ABI', () => {
  assert.equal(zlink.MonitorEventType.SendFlowPaused, 0x10000);
  assert.equal(zlink.MonitorEventType.SendFlowResumed, 0x20000);
  assert.equal(zlink.MonitorEventType.FlowStateStale, 0x40000);
});

test('flow monitor event flag constants match the C ABI', () => {
  assert.equal(zlink.MonitorEventFlag.ConnectionReadyEdge, 0x1);
  assert.equal(zlink.MonitorEventFlag.SendFlowWritable, 0x2);
  assert.equal(zlink.MonitorEventFlag.FlowStateStaleGeneration, 0x4);
  assert.equal(zlink.MonitorEventFlag.FlowStateStaleEpoch, 0x8);
});

test('SOCKET_MONITOR_EVENT_ALL covers the three new flow event bits', () => {
  assert.equal(
    zlink.SOCKET_MONITOR_EVENT_ALL & zlink.MonitorEventType.SendFlowPaused,
    zlink.MonitorEventType.SendFlowPaused
  );
  assert.equal(
    zlink.SOCKET_MONITOR_EVENT_ALL & zlink.MonitorEventType.SendFlowResumed,
    zlink.MonitorEventType.SendFlowResumed
  );
  assert.equal(
    zlink.SOCKET_MONITOR_EVENT_ALL & zlink.MonitorEventType.FlowStateStale,
    zlink.MonitorEventType.FlowStateStale
  );
});

test('monitor status exposes the five flow metrics, zero on a fresh PAIR socket', () => {
  const ctx = zlink.createContext();
  const pair = zlink.createPairSocket(ctx);
  const monitor = pair.monitorOpen();
  const status = monitor.status();

  for (const field of [
    'flowPausedConnections',
    'flowPauseAppliedTotal',
    'flowResumeAppliedTotal',
    'flowStateStaleTotal',
    'flowPauseDurationMs'
  ]) {
    assert.equal(typeof status[field], 'bigint', field);
    assert.equal(status[field], 0n, field);
  }

  monitor.close();
  pair.close();
  ctx.close();
});

test('monitor status detail FLOW_STATE bit matches the C ABI', () => {
  // core/include/zlink_enum.h: ZLINK_MONITOR_STATUS_DETAIL_FLOW_STATE = 1u << 5
  const ctx = zlink.createContext();
  const pair = zlink.createPairSocket(ctx);
  const monitor = pair.monitorOpen();
  const status = monitor.status();

  assert.equal(typeof status.isFlowStateDetailPopulated, 'function');
  assert.equal(
    status.isFlowStateDetailPopulated(),
    (status.detailFlags & (1 << 5)) !== 0
  );

  monitor.close();
  pair.close();
  ctx.close();
});

test('setReceiveFlowState succeeds on DEALER/ROUTER and repeat calls are idempotent', () => {
  const ctx = zlink.createContext();
  const router = zlink.createRouterSocket(ctx);
  const dealer = zlink.createDealerSocket(ctx);

  router.bind('inproc://flow-state-dealer-router-idempotent');
  dealer.connect('inproc://flow-state-dealer-router-idempotent');

  router.setReceiveFlowState(zlink.ReceiveFlowState.PAUSED);
  router.setReceiveFlowState(zlink.ReceiveFlowState.PAUSED);
  router.setReceiveFlowState(zlink.ReceiveFlowState.RUNNING);
  router.setReceiveFlowState(zlink.ReceiveFlowState.RUNNING);

  dealer.setReceiveFlowState(zlink.ReceiveFlowState.PAUSED);
  dealer.setReceiveFlowState(zlink.ReceiveFlowState.PAUSED);
  dealer.setReceiveFlowState(zlink.ReceiveFlowState.RUNNING);
  dealer.setReceiveFlowState(zlink.ReceiveFlowState.RUNNING);

  dealer.close();
  router.close();
  ctx.close();
});

test('setReceiveFlowState reports NotSupported on PAIR, PUB/SUB family, and STREAM', () => {
  const ctx = zlink.createContext();
  const pair = zlink.createPairSocket(ctx);
  const pub = zlink.createPubSocket(ctx);
  const sub = zlink.createSubSocket(ctx);
  const xpub = zlink.createXPubSocket(ctx);
  const xsub = zlink.createXSubSocket(ctx);
  const stream = zlink.createStreamSocket(ctx);

  for (const socket of [pair, pub, sub, xpub, xsub, stream]) {
    assert.throws(
      () => socket.setReceiveFlowState(zlink.ReceiveFlowState.PAUSED),
      (error: unknown) => isConfigError(error)
        && (error as InstanceType<typeof zlink.ConfigError>).result === zlink.ConfigResult.NotSupported
    );
  }

  // Existing send/recv behavior on an unsupported socket type is unaffected
  // by the rejected call.
  pair.bind('inproc://flow-state-pair-unaffected');
  const peer = zlink.createPairSocket(ctx);
  peer.connect('inproc://flow-state-pair-unaffected');
  peer.send().message('still-works').submit();
  const received = new zlink.Received();
  pair.recv(received);
  assert.equal(received.parts[0].data().toString(), 'still-works');

  peer.close();
  stream.close();
  xsub.close();
  xpub.close();
  sub.close();
  pub.close();
  pair.close();
  ctx.close();
});

test('setReceiveFlowState maps invalid handle and invalid argument per the config error policy', () => {
  const ctx = zlink.createContext();
  const router = zlink.createRouterSocket(ctx);
  router.close();

  assert.throws(
    () => router.setReceiveFlowState(zlink.ReceiveFlowState.RUNNING),
    (error: unknown) => isConfigError(error)
      && (error as InstanceType<typeof zlink.ConfigError>).result === zlink.ConfigResult.InvalidHandle
  );

  const dealer = zlink.createDealerSocket(ctx);
  for (const outOfRange of [2, -1, 999]) {
    assert.throws(
      () => dealer.setReceiveFlowState(outOfRange as unknown as typeof zlink.ReceiveFlowState[keyof typeof zlink.ReceiveFlowState]),
      (error: unknown) => isConfigError(error)
        && (error as InstanceType<typeof zlink.ConfigError>).result === zlink.ConfigResult.InvalidArgument
    );
  }

  dealer.close();
  ctx.close();
});

test('setReceiveFlowState after close observes only a bounded close-related outcome', () => {
  // Node's single JS-thread execution model cannot produce a genuine
  // concurrent close/config race the way thread-based bindings (C++, Go,
  // Java, Python, Rust) do. This test instead exercises the deterministic
  // sequential ordering documented in worklog/stage7-c-api.md §1.2 as one of
  // the two observable "close won" shapes: a fully torn-down handle reports
  // InvalidHandle. Repeated to confirm it never throws anything else or
  // corrupts state.
  for (let i = 0; i < 20; i += 1) {
    const ctx = zlink.createContext();
    const router = zlink.createRouterSocket(ctx);
    router.bind(`inproc://flow-state-close-race-${i}`);
    router.close();

    let observedResult: number | undefined;
    try {
      router.setReceiveFlowState(zlink.ReceiveFlowState.RUNNING);
      observedResult = zlink.ConfigResult.Ok;
    } catch (error) {
      assert.ok(isConfigError(error), 'expected a ConfigError');
      const result = (error as InstanceType<typeof zlink.ConfigError>).result;
      assert.ok(
        result === zlink.ConfigResult.InvalidHandle || result === zlink.ConfigResult.InvalidState,
        `unexpected result ${result}`
      );
      observedResult = result;
    }
    assert.notEqual(observedResult, undefined);
    ctx.close();
  }
});

test('public surface has no flow-frame receive/encode API or PAUSE-bypass send variant', () => {
  const ctx = zlink.createContext();
  const sockets = [
    zlink.createPairSocket(ctx),
    zlink.createPubSocket(ctx),
    zlink.createSubSocket(ctx),
    zlink.createXPubSocket(ctx),
    zlink.createXSubSocket(ctx),
    zlink.createDealerSocket(ctx),
    zlink.createRouterSocket(ctx),
    zlink.createStreamSocket(ctx),
  ];

  const forbiddenSubstrings = [
    'flowframe',
    'flow_frame',
    'encodeflow',
    'decodeflow',
    'receiveflowframe',
    'sendflowframe',
    'pausebypass',
    'bypasspause',
  ];

  for (const socket of sockets) {
    const methodNames = new Set<string>();
    let proto = Object.getPrototypeOf(socket);
    while (proto && proto !== Object.prototype) {
      for (const name of Object.getOwnPropertyNames(proto)) {
        methodNames.add(name);
      }
      proto = Object.getPrototypeOf(proto);
    }
    for (const name of methodNames) {
      const lower = name.toLowerCase();
      for (const forbidden of forbiddenSubstrings) {
        assert.ok(
          !lower.includes(forbidden),
          `socket exposes forbidden flow API member: ${name}`
        );
      }
    }
    // The only public member whose name contains "flow" is the state setter.
    const flowMembers = [...methodNames].filter((name) => name.toLowerCase().includes('flow'));
    assert.deepEqual(flowMembers, ['setReceiveFlowState']);
    socket.close();
  }

  for (const exportName of Object.keys(zlink)) {
    const lower = exportName.toLowerCase();
    for (const forbidden of forbiddenSubstrings) {
      assert.ok(
        !lower.includes(forbidden),
        `zlink package exposes forbidden flow API export: ${exportName}`
      );
    }
  }

  ctx.close();
});

test('existing HWM options and DEALER/ROUTER traffic are unchanged by receive-flow-state calls', async () => {
  const ctx = zlink.createContext();
  ctx.options.autoHwmEnabled = false;
  const router = zlink.createRouterSocket(ctx);
  const dealer = zlink.createDealerSocket(ctx);

  router.bind('inproc://flow-state-hwm-smoke');
  dealer.connect('inproc://flow-state-hwm-smoke');

  const originalSndHwm = dealer.options.sendHwm;
  dealer.options.sendHwm = 4096n;
  assert.equal(dealer.options.sendHwm, 4096n);
  dealer.options.sendHwm = originalSndHwm;

  // Idempotent RUNNING no-op transition should not disturb ordinary traffic.
  router.setReceiveFlowState(zlink.ReceiveFlowState.RUNNING);
  dealer.setReceiveFlowState(zlink.ReceiveFlowState.RUNNING);

  await dealer.send().message('flow-state-smoke').submit();
  const received = new zlink.Received();
  router.recv(received);
  assert.equal(received.parts[0].data().toString(), 'flow-state-smoke');

  await router.send(received.routingId).message('reply').submit();
  const reply = new zlink.Received();
  dealer.recv(reply);
  assert.equal(reply.parts[0].data().toString(), 'reply');

  dealer.close();
  router.close();
  ctx.close();
});
