const assert = require('node:assert/strict');
const test = require('node:test');
const framework = require('../../packages/framework/dist/internal');
const { createSpotHandle } = require('../../packages/framework/dist/runtime/spots/spot-handle');
const {
  decodeChannelHeader,
  encodeChannelReplyParts
} = require('../../packages/framework/dist/runtime/channels/channel-envelope');
const { ZLinkBufferMessage } =
  require('../../packages/framework/dist/runtime/backend/runtime-message');
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

function createResolvedSpot(spotKind) {
  return createSpotHandle(
    'spot',
    { meshName: 'mesh', nodeRid: 'peer', spotId: 'spot', spotKind },
    async () => {
      throw new Error('Resolved handle must not refresh.');
    }
  );
}

function assertSpotRouteTarget(target, spotKind) {
  assert.equal(target.routerChannelId, 'mesh');
  assert.equal(target.targetNodeRid, 'peer');
  assert.equal(target.spotId, 'spot');
  assert.equal(target.spotKind, spotKind);
}

for (const spotKind of [framework.ZLinkSpotKind.Entry, framework.ZLinkSpotKind.User]) {
  test(`runtime route transport: ${spotKind} send/request without Mesh runtime`, async () => {
    const registration = framework.createFrameworkRegistration();
    const sendTargets = [];
    const requestTargets = [];
    const manager = {
      async routeSendToSpot(target) {
        sendTargets.push(target);
      },
      async routeRequestToSpot(
        target,
        _packetName,
        _request,
        _timeoutMs,
        _signal,
        _metadata,
        onAdmitted,
        requestOperation,
        receiver
      ) {
        requestTargets.push(target);
        onAdmitted?.();
        receiver.operationCompleted(requestOperation.id, 'reply', undefined);
      }
    };
    const transport = new framework.ZLinkRuntimeRouteTransport(
      () => manager,
      undefined,
      () => undefined
    );
    const client = new framework.DefaultZLinkRouteClient(registration, transport);
    const spot = createResolvedSpot(spotKind);

    await client.sendToSpot(spot, new Notice()).submit();
    const request = client.requestToSpot(spot, new Notice()).submit();
    assert.equal(await request, 'reply');
    await request.admitted;

    assert.equal(sendTargets.length, 1);
    assert.equal(requestTargets.length, 1);
    assertSpotRouteTarget(sendTargets[0], spotKind);
    assertSpotRouteTarget(requestTargets[0], spotKind);
  });
}

function createHostBackend() {
  const backend = {
    meshAdapterCalls: 0,
    requestTargets: [],
    routerFactoryCalls: 0,
    router: undefined,
    sendTargets: []
  };
  const context = {
    async dispose() {},
    getCoreHwmBudgetSnapshot() {
      return undefined;
    }
  };
  const channelAdapter = {
    createContext() {
      return context;
    },
    createTopicMessage() {
      throw new Error('Unexpected channel topic message creation.');
    },
    createDealerSocket() {
      throw new Error('Unexpected DEALER socket creation.');
    },
    createRouterSocket() {
      backend.routerFactoryCalls += 1;
      const router = {
        channelName: undefined,
        maxMessageSize: 0,
        peerWeight: 0,
        receiveFlowState: undefined,
        receiveHighWaterMark: 0,
        receiveTimeoutMs: 0,
        routingId: undefined,
        sendHighWaterMark: 0,
        setChannelName(channelName) {
          router.channelName = channelName;
        },
        setProbe() {
          throw new Error('Unexpected route probe configuration.');
        },
        setReceiveFlowState(state) {
          router.receiveFlowState = state;
        },
        setRoutingId(routingId) {
          router.routingId = String(routingId);
        },
        sendToSpot(targetNodeRid, targetSpotId) {
          backend.sendTargets.push({
            nodeRid: String(targetNodeRid),
            spotId: String(targetSpotId)
          });
          return Promise.resolve();
        },
        requestToSpot(
          targetNodeRid,
          targetSpotId,
          parts,
          _timeoutMs,
          onAdmitted,
          requestOperation,
          receiver
        ) {
          backend.requestTargets.push({
            nodeRid: String(targetNodeRid),
            spotId: String(targetSpotId)
          });
          const requestHeader = decodeChannelHeader(
            parts.map((part) => ZLinkBufferMessage.from(part))
          );
          const replyParts = encodeChannelReplyParts(requestHeader, 'reply').map((part) =>
            ZLinkBufferMessage.from(part)
          );
          onAdmitted?.();
          receiver.operationCompleted(requestOperation.id, replyParts, undefined);
        },
        dispose() {}
      };
      backend.router = router;
      return router;
    },
    createPublisherSocket() {
      throw new Error('Unexpected PUB socket creation.');
    },
    createSubscriberSocket() {
      throw new Error('Unexpected SUB socket creation.');
    },
    createReadablePoller() {
      throw new Error('Unexpected readable poller creation.');
    }
  };
  const backendAdapterFactory = {
    createTopicMessage() {
      throw new Error('Unexpected backend topic message creation.');
    },
    createChannelAdapter() {
      return channelAdapter;
    },
    createMeshAdapter() {
      backend.meshAdapterCalls += 1;
      throw new Error('Unexpected Mesh adapter creation.');
    },
    createStreamAdapter() {
      throw new Error('Unexpected stream adapter creation.');
    },
    createMonitoringAdapter() {
      return {
        openSocketMonitor() {
          return {
            onEvent() {},
            dispose() {}
          };
        }
      };
    }
  };

  return { backend, backendAdapterFactory };
}

for (const spotKind of [framework.ZLinkSpotKind.Entry, framework.ZLinkSpotKind.User]) {
  test(`started host routes public ${spotKind} send and request via route channel`, async () => {
    const registration = framework.createFrameworkRegistration({ routeChannels: ['mesh'] });
    const { backend, backendAdapterFactory } = createHostBackend();
    const host = new framework.ZLinkFrameworkRuntimeHost(
      { registration },
      { backendAdapterFactory }
    );
    const client = new framework.DefaultZLinkRouteClient(registration, host.routeTransport);
    const spot = createResolvedSpot(spotKind);

    try {
      await host.start();
      assert.equal(host.status.state, framework.ZLinkFrameworkRuntimeState.Serving);

      await client.sendToSpot(spot, new Notice()).submit();
      const request = client.requestToSpot(spot, new Notice()).submit();
      assert.equal(await request, 'reply');
      await request.admitted;
    } finally {
      await host.stop();
    }

    assert.equal(host.status.state, framework.ZLinkFrameworkRuntimeState.Stopped);
    assert.equal(backend.routerFactoryCalls, 1);
    assert.equal(backend.meshAdapterCalls, 0);
    assert.equal(backend.sendTargets.length, 1);
    assert.equal(backend.requestTargets.length, 1);
    assert.deepEqual(backend.sendTargets, [{ nodeRid: 'peer', spotId: 'spot' }]);
    assert.deepEqual(backend.requestTargets, [{ nodeRid: 'peer', spotId: 'spot' }]);
    assert.equal(backend.router.channelName, 'mesh');
  });
}
