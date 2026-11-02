'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const zlink = require('@zlink-systems/zlink');

const framework = require('../../packages/framework/dist/internal');

const meshName = 'route-mesh-router-hwm';

function registration(configureRouterSocket) {
  return framework.createFrameworkRegistration(
    framework.createFrameworkOptions((builder) => {
      builder
        .configureInboundDispatch()
        .coreHwmProfile(framework.ZLinkCoreHwmProfile.Balanced);
      const socket = builder.addRouteMesh(meshName).configureRouterSocket();
      configureRouterSocket?.(socket);
    })
  );
}

async function startAndReadSocket(configureRouterSocket) {
  const host = new framework.ZLinkFrameworkRuntimeHost({
    registration: registration(configureRouterSocket)
  });
  const originalCreateRouterSocket = zlink.createRouterSocket;
  const routerSockets = [];
  zlink.createRouterSocket = (context) => {
    const socket = originalCreateRouterSocket(context);
    routerSockets.push(socket);
    return socket;
  };

  let dealer;
  let monitor;
  try {
    await host.start();
    const endpoint = host.getListenerStatus('routeMesh', meshName).endpoint;
    const router = routerSockets.find((socket) => socket.options.lastEndpoint === endpoint);
    assert.ok(router, 'RouteMesh listener uses a binding ROUTER socket');
    const bindingContext = host.context.nativeInstance;
    dealer = zlink.createDealerSocket(bindingContext);
    monitor = dealer.monitorOpen([zlink.MonitorEventType.ConnectionReady]);
    dealer.connect(endpoint);
    assert.equal(monitor.recv()?.event, zlink.MonitorEventType.ConnectionReady);
    bindingContext.recalculateAutoHwm();
    const coreSnapshot = bindingContext.getCoreHwmBudgetSnapshot();
    return {
      sendHighWaterMark: router.options.sendHwm,
      receiveHighWaterMark: router.options.recvHwm,
      appliedHwmBytes: host.status.capacity.coreHwm.totalAppliedHwmBytes,
      activeSendQueueCount: coreSnapshot.activeSendQueueCount,
      activeReceiveQueueCount: coreSnapshot.activeReceiveQueueCount
    };
  } finally {
    monitor?.close();
    dealer?.close();
    zlink.createRouterSocket = originalCreateRouterSocket;
    await host.stop();
  }
}

test('RouteMesh configureRouterSocket applies both HWM directions to its ROUTER', async () => {
  const baseline = await startAndReadSocket();
  const configured = await startAndReadSocket((socket) => {
    socket.sendHighWaterMark = 17;
    socket.receiveHighWaterMark = 23;
  });

  assert.equal(configured.sendHighWaterMark, 17n);
  assert.equal(configured.receiveHighWaterMark, 23n);
  assert.ok(configured.appliedHwmBytes > 0n);
  assert.notEqual(
    configured.appliedHwmBytes,
    baseline.appliedHwmBytes,
    `configured applied=${configured.appliedHwmBytes}; baseline applied=${baseline.appliedHwmBytes}`
  );
  assert.ok(configured.activeSendQueueCount + configured.activeReceiveQueueCount > 0n);
});
