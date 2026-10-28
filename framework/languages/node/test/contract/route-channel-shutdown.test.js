const assert = require('node:assert/strict');
const test = require('node:test');
const framework = require('../../packages/framework/dist/internal');
const { createSpotHandle } = require('../../packages/framework/dist/runtime/spots/spot-handle');
class Notice {}

function fixture(spotKind) {
  const registration = framework.createFrameworkRegistration();
  // Empty registration and a fake context keep this lifecycle contract socket-free.
  const host = new framework.ZLinkFrameworkRuntimeHost({ registration }, {
    backendAdapterFactory: {
      createMonitoringAdapter() { return {}; },
      createChannelAdapter() {
        return { createContext: () => ({
          async dispose() {},
          getCoreHwmBudgetSnapshot() { return undefined; }
        }) };
      }
    }
  });
  const client = new framework.DefaultZLinkRouteClient(registration, host.routeTransport);
  const spot = createSpotHandle('spot', {
    meshName: 'mesh', nodeRid: 'peer', spotId: 'spot', spotKind
  }, async () => { throw new Error('Resolved handle must not refresh.'); });
  return { host, client, spot };
}

async function assertCall(client, spot, operation, accepts) {
  if (operation === 'send') {
    await assert.rejects(client.sendToSpot(spot, new Notice()).submit(), accepts);
    return;
  }
  const request = client.requestToSpot(spot, new Notice()).submit();
  await Promise.all([
    assert.rejects(request, accepts),
    assert.rejects(request.admitted, accepts)
  ]);
}

for (const spotKind of [framework.ZLinkSpotKind.Entry, framework.ZLinkSpotKind.User, framework.ZLinkSpotKind.Instance]) {
  for (const operation of ['send', 'request']) {
    test(`stopped route channel rejects public ${spotKind} ${operation} with ShuttingDown`, async () => {
      const { host, client, spot } = fixture(spotKind);
      try {
        await host.start();
        await host.stop();
        assert.equal(host.status.state, framework.ZLinkFrameworkRuntimeState.Stopped);
        await assertCall(client, spot, operation, error => {
          assert(error instanceof framework.ZLinkFrameworkException, `${error.name}: ${error.message}`);
          assert.equal(error.kind, framework.ZLinkFrameworkErrorKind.ShuttingDown);
          return true;
        });
      } finally {
        await host.stop();
      }
    });

    test(`unstarted route channel preserves its ${spotKind} ${operation} configuration error`, async () => {
      const { host, client, spot } = fixture(spotKind);
      assert.equal(host.status.state, framework.ZLinkFrameworkRuntimeState.Preparing);
      await assertCall(client, spot, operation, error => {
        assert(error instanceof framework.ZLinkConfigurationException);
        return true;
      });
    });
  }
}

for (const spotKind of [framework.ZLinkSpotKind.Entry, framework.ZLinkSpotKind.User]) {
  test(`manager route without mesh runtime preserves public ${spotKind} send and request`, async () => {
    const { host, client, spot } = fixture(spotKind);
    let sends = 0;
    let requests = 0;
    host.channelRuntime = {
      async routeSendToSpot() { sends += 1; },
      async routeRequestToSpot() { requests += 1; return 'reply'; }
    };
    await client.sendToSpot(spot, new Notice()).submit();
    const request = client.requestToSpot(spot, new Notice()).submit();
    assert.equal(await request, 'reply');
    await request.admitted;
    assert.equal(sends, 1);
    assert.equal(requests, 1);
  });
}
