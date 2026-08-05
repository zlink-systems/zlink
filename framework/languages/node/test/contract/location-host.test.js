const assert = require('node:assert/strict');
const test = require('node:test');

const zlink = require('@zlink-systems/zlink');
const framework = require('../../packages/framework/dist/internal');
const flowContext = require('../../packages/framework/dist/runtime/diagnostics/flow-context');
const nestjs = require('../../packages/nestjs/dist');

test('framework and NestJS builders register separate location and relocation stores', async () => {
  const store = new framework.ZLinkInMemoryProviderLocationStore();
  const relocationStore = {};

  const options = framework.createFrameworkOptions((builder) => {
    builder.addLocationStore(store);
    builder.addRelocationStore(relocationStore);
    builder.configureLocations().ownerLeaseRenewIntervalMs(123);
  });
  const registration = framework.createFrameworkRegistration(options);
  assert.equal(registration.locations.storeInstance, store);
  assert.equal(registration.locations.relocationStoreInstance, relocationStore);
  assert.equal(registration.locations.options.ownerLeaseRenewIntervalMs, 123);

  const nestBuilder = nestjs.zlinkFramework()
    .addLocationStore(store)
    .addRelocationStore(relocationStore);
  nestBuilder.configureLocations()
    .ownerLeaseRenewIntervalMs(100)
    .ownerLeaseRenewTimeoutMs(30)
    .ownerLeaseFencingMarginMs(50)
    .ownerLeaseTtlMs(456);
  const nestModule = nestjs.ZLinkModule.forRoot(nestBuilder.build());
  const nestRegistration = await resolveFrameworkRegistration(nestModule);
  assert.equal(nestRegistration.locations.storeInstance, store);
  assert.equal(nestRegistration.locations.relocationStoreInstance, relocationStore);
  assert.equal(nestRegistration.locations.options.ownerLeaseTtlMs, 456);

});

test('framework runtime host uses the explicit location store for Actor lifecycle and authority routing', () => {
  const store = new framework.ZLinkInMemoryProviderLocationStore();
  const runtime = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration({
      locations: {
        storeInstance: store
      }
    })
  });

  const actorOptions = runtime.createActorManagerOptions();
  const spotOptions = runtime.createSpotManagerOptions();

  assert.equal(typeof actorOptions.locationLifecycle, 'object');
  assert.equal(spotOptions.locationLifecycle, undefined);
  assert.equal(typeof spotOptions.spotRouteResolver?.resolve, 'function');
});

test('framework host republishes the current lifecycle state after owner lease recovery', async () => {
  const host = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration()
  });
  const firstToken = { ownerId: 'owner-a', leaseGeneration: 1n };
  const secondToken = { ownerId: 'owner-a', leaseGeneration: 2n };
  let currentToken = firstToken;
  let recoveryHandler;
  const runtime = {
    get currentOwnerToken() {
      return currentToken;
    },
    addOwnerLeaseRenewedHandler(handler) {
      recoveryHandler = handler;
    },
    removeOwnerLeaseRenewedHandler() {}
  };
  const publishedStates = [];
  const spotNodeRuntime = {
    async publishMeshNodeState(state) {
      publishedStates.push(state);
    }
  };

  host.runtimeState = framework.ZLinkFrameworkRuntimeState.Draining;
  host.installOwnerLeaseRecoveryPublication(runtime, spotNodeRuntime);
  currentToken = secondToken;
  recoveryHandler();
  await new Promise((resolve) => setImmediate(resolve));

  assert.deepEqual(publishedStates, [framework.ZLinkFrameworkRuntimeState.Draining]);
  host.removeOwnerLeaseRecoveryPublication();
});

test('framework runtime host starts location runtime and injects lifecycle into managers', async () => {
  const now = () => new Date(Date.UTC(2026, 6, 3, 0, 0, 0));
  const provider = new framework.ZLinkInMemoryProviderLocationStore(now);
  const store = new framework.ZLinkLocationStoreRepository(provider, now);
  const calls = [];
  const nodeRid = rid('node-a');
  const runtime = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration({
      locations: {
        storeInstance: provider,
        options: {
          ownerLeaseRenewIntervalMs: 1000
        }
      },
      spotNodes: {
        play: {
          router: { bind: 'tcp://127.0.0.1:9101', routingId: 'node-a' }
        }
      }
    })
  }, {
    backendAdapterFactory: fakeBackendAdapterFactory(calls, nodeRid)
  });

  const preStartActorOptions = runtime.createActorManagerOptions();
  const preStartSpotOptions = runtime.createSpotManagerOptions();
  const ownerId = runtime.locationOwner.currentRuntime.ownerId;
  assert.equal(typeof preStartActorOptions.locationLifecycle, 'object');
  assert.equal(preStartSpotOptions.locationLifecycle, undefined);
  assert.equal((await store.readOwnerLease(ownerId)).kind, 'missing');

  await runtime.start();

  const lease = await store.readOwnerLease(ownerId);
  assert.equal(lease.kind, 'found');

  const actorOptions = runtime.createActorManagerOptions();
  const spotOptions = runtime.createSpotManagerOptions();
  assert.equal(actorOptions.locationLifecycle, preStartActorOptions.locationLifecycle);
  assert.equal(spotOptions.locationLifecycle, undefined);
  assert.equal(String(spotOptions.nodeRidProvider('play')), 'node-a');
  assert.equal(typeof spotOptions.nodeGenerationProvider('play'), 'bigint');
  assert.equal(typeof spotOptions.spotRouteResolver?.resolve, 'function');

  await actorOptions.locationLifecycle.claimActor('player', 'actor-1', nodeRid);
  assert.notEqual(await store.resolveActor({ meshName: 'play', actorId: 'actor-1' }), undefined);

  await runtime.stop();

  assert.equal((await store.readOwnerLease(ownerId)).kind, 'missing');
  assert.equal(await store.resolveActor({ meshName: 'play', actorId: 'actor-1' }), undefined);
  assert.deepEqual(calls, [
    'spot:dispose',
    'context:dispose'
  ]);
});

test('framework host startup begins a lifecycle flow', async () => {
  const calls = [];
  const backendAdapterFactory = fakeBackendAdapterFactory(calls, rid('lifecycle-node'));
  const createChannelAdapter = backendAdapterFactory.createChannelAdapter;
  let startupFlow;
  backendAdapterFactory.createChannelAdapter = () => {
    startupFlow = flowContext.currentOrCreateFlow('Lifecycle', false);
    return createChannelAdapter();
  };
  const runtime = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration({
      dispatch: {
        diagnostics: {
          messageFlow: framework.ZLinkMessageFlowLogMode.KeyTransitions,
          sampleRate: 1,
          includeMessageSizes: false
        }
      }
    })
  }, { backendAdapterFactory });

  await runtime.start();
  await runtime.stop();

  assert.equal(startupFlow?.flowOrigin, 'Lifecycle');
  assert.match(startupFlow?.flowId ?? '', /^[0-9a-f-]{36}$/);
});

test('framework runtime host starts channel auto-connect loops from location peers', async () => {
  const now = () => new Date(Date.UTC(2026, 6, 3, 0, 0, 0));
  const provider = new framework.ZLinkInMemoryProviderLocationStore(now);
  const store = new framework.ZLinkLocationStoreRepository(provider, now);
  const calls = [];
  const nodeRid = rid('node-a');

  const remoteOwner = await store.claimOwnerLease('owner-remote', 30000);
  assert.equal(remoteOwner.kind, 'claimed');
  await store.updateClientServer(
    clientServerDescriptor(remoteOwner.token, 'api', 'node-b', 'tcp://remote-api'),
    framework.ZLinkLocationWriteIntent.NewClaim
  );
  await store.updatePeer(
    routePeer('manual-mesh', 'owner-remote', 'node-b', 'tcp://remote-manual-route'),
    framework.ZLinkLocationWriteIntent.NewClaim
  );
  await store.updatePeer(
    spotPeer('owner-remote', 'node-b', 'tcp://remote-spot'),
    framework.ZLinkLocationWriteIntent.NewClaim
  );
  await store.updateFanoutPublisher(
    fanoutDescriptor(remoteOwner.token, 'events', 'node-b', 'tcp://remote-events'),
    framework.ZLinkLocationWriteIntent.NewClaim
  );

  const runtime = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration({
      locations: {
        storeInstance: provider,
        options: {
          ownerLeaseRenewIntervalMs: 1000,
          pollingIntervalMs: 1000
        }
      },
      channels: {
        api: {
          client: {}
        },
        manual: {
          client: {
            manualConnections: ['tcp://manual']
          }
        },
        events: {
          subscriber: {},
          publishHandlers: [{ packetName: 'Event', handler: { async handle() {} } }]
        },
        'local-events': {
          routingId: 'node-a',
          publisher: { bind: 'tcp://local-events' }
        },
        'manual-events': {
          subscriber: { manualConnections: ['tcp://manual-events'] },
          publishHandlers: [{ packetName: 'Event', handler: { async handle() {} } }]
        }
      },
      routeChannels: [{
        routerChannelId: 'mesh',
        bind: 'tcp://local-route',
        routingId: 'node-a'
      }, {
        routerChannelId: 'manual-mesh',
        bind: 'tcp://local-manual-route',
        routingId: 'node-a',
        manualConnections: ['tcp://manual-route']
      }],
      spotNodes: {
        play: {
          router: { bind: 'tcp://127.0.0.1:9101', routingId: 'node-a' }
        }
      }
    })
  }, {
    backendAdapterFactory: fakeBackendAdapterFactory(calls, nodeRid)
  });

  try {
    await runtime.start();
    assert.deepEqual(
      calls.filter((call) =>
        (call.startsWith('dealer:') || call.startsWith('router:') || call.startsWith('subscriber:'))
        && call.includes(':connect:')).sort(),
      [
        'dealer:api:connect:tcp://remote-api',
        'dealer:manual:connect:tcp://manual',
        'router:manual-mesh:connect:tcp://manual-route',
        'subscriber:manual-events:connect:tcp://manual-events',
        'subscriber:events:connect:tcp://remote-events'
      ].sort()
    );
    const publisherRows = await store.listFanoutPublishers('local-events');
    assert.equal(publisherRows.items.length, 1);
    assert.equal(String(publisherRows.items[0].publisherRid), 'node-a');
  } finally {
    await runtime.stop();
  }
  assert.ok(calls.includes('dealer:api:disconnect:tcp://remote-api'));
  assert.ok(calls.includes('subscriber:events:disconnect:tcp://remote-events'));
  assert.equal(calls.some((call) => call.includes('tcp://remote-manual')), false);
  assert.equal(
    calls.filter((call) => call === 'dealer:manual:disconnect:tcp://manual').length,
    0
  );
});

test('manual Mesh router connection suppresses only the matching store-driven route', async () => {
  const now = () => new Date(Date.UTC(2026, 6, 3, 0, 0, 0));
  const provider = new framework.ZLinkInMemoryProviderLocationStore(now);
  const store = new framework.ZLinkLocationStoreRepository(provider, now);
  const calls = [];
  const nodeRid = rid('node-a');

  await store.claimOwnerLease('owner-remote', 30000);
  await store.updatePeer(
    spotPeer('owner-remote', 'node-b', 'tcp://remote-spot'),
    framework.ZLinkLocationWriteIntent.NewClaim
  );
  await store.updatePeer(
    spotPeer('owner-lower', 'node-0', 'tcp://lower-spot'),
    framework.ZLinkLocationWriteIntent.NewClaim
  );
  await store.updatePeer(
    spotPeer('owner-remote', 'node-c', 'tcp://manual-spot'),
    framework.ZLinkLocationWriteIntent.NewClaim
  );

  const runtime = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration({
      locations: {
        storeInstance: provider,
        options: {
          ownerLeaseRenewIntervalMs: 1000,
          pollingIntervalMs: 1000
        }
      },
      spotNodes: {
        play: {
          router: {
            bind: 'tcp://local-spot',
            routingId: 'node-a',
            manualConnections: ['tcp://manual-spot']
          },
          pubSub: {
            bind: 'tcp://local-pub',
            routingId: 'node-a'
          }
        }
      }
    })
  }, {
    backendAdapterFactory: fakeBackendAdapterFactory(calls, nodeRid)
  });

  try {
    await runtime.start();
    await new Promise((resolve) => setImmediate(resolve));
    assert.equal(calls.some((call) => call.includes('tcp://remote-spot')), false);
    assert.equal(calls.some((call) => call.includes('tcp://remote-pub')), false);
    assert.equal(calls.some((call) => call.includes('tcp://lower-spot')), false);
    assert.equal(calls.some((call) => call.includes('tcp://lower-pub')), false);
    assert.ok(calls.includes('spot:connectPeer:tcp://manual-spot'));
  } finally {
    await runtime.stop();
  }

  assert.equal(calls.filter((call) => call === 'spot:disconnectPeer:tcp://manual-spot').length, 0);
});

async function resolveFrameworkRegistration(module) {
  const provider = module.providers.find((entry) => entry.provide === nestjs.ZLINK_FRAMEWORK_REGISTRATION);
  if ('useValue' in provider) {
    return provider.useValue;
  }
  return await provider.useFactory();
}

function rid(value) {
  return zlink.RoutingId.from(value);
}

function fakeBackendAdapterFactory(calls, nodeRid) {
  return {
    createChannelAdapter() {
      return {
        createContext() {
          return {
            nativeInstance: {},
            shutdown() {},
            async dispose() {
              calls.push('context:dispose');
            }
          };
        },
        createDealerSocket() {
          return fakeConnectableSocket(calls, 'dealer');
        },
        createRouterSocket() {
          const socket = fakeConnectableSocket(calls, 'router');
          return Object.assign(socket, {
            setRoutingId() {},
            recv() { return null; },
            reply() { return { message() { return this; }, submit() {} }; }
          });
        },
        createPublisherSocket() {
          return {
            nativeInstance: {},
            setChannelName(channelName) { this.channelName = channelName; },
            bind(endpoint) { calls.push(`publisher:${this.channelName ?? ''}:bind:${endpoint}`); },
            onSendReady() {},
            publish() { return true; },
            async dispose() {}
          };
        },
        createSubscriberSocket() {
          return {
            ...fakeConnectableSocket(calls, 'subscriber'),
            setSubscription() {},
            subscribe() { return false; }
          };
        },
        createReadablePoller() {
          return {
            wait() { return false; },
            dispose() {}
          };
        },
        createTopicMessage() {
          return { parts: [] };
        }
      };
    },
    createMeshAdapter() {
      return {
        createMeshNode(_context, options) {
          let routingId = options.routingId === undefined ? String(nodeRid) : String(options.routingId);
          let nextConnectionIntent = 1n;
          return {
            nativeInstance: {},
            setRoutingId(value) { routingId = String(value); },
            setBind() {},
            addChannelName() {},
            setChannelWeight() {},
            configureObjectPlacement() {},
            createPublisher() { return { close() {} }; },
            start() {},
            setReadyHandler() {},
            createReadyBatch() { return fakeReadyBatch(); },
            createReceiveBatch() { return fakeReceiveBatch(); },
            drainReady() { return { ok: false, hasResidue: false, records: [] }; },
            connectPeer({ endpoint, expectedRid }) {
              calls.push(`spot:connectPeer:${endpoint}`);
              if (expectedRid !== undefined) {
                calls.push(`spot:connectPeerRid:${expectedRid.toHex()}:${endpoint}`);
              }
              return nextConnectionIntent++;
            },
            removePeerConnection(connectionIntent) {
              calls.push(`spot:removePeerConnection:${connectionIntent}`);
            },
            disconnectPeer(peerRid) { calls.push(`spot:disconnectPeerRid:${peerRid.toHex()}`); },
            status() {
              return {
                routingId,
                lifecycleGeneration: 3n,
                descriptorRevision: 1n,
                localEndpoint: 'tcp://127.0.0.1:9101',
                state: 3,
                lastChangedMs: 1n,
                pendingApplicationMessages: 0n,
                pendingInfrastructureMessages: 0n
              };
            },
            peers() { return []; },
            shutdown() {},
            close() {
              calls.push('spot:dispose');
            }
          };
        }
      };
    },
    createStreamAdapter() {
      return {
        createStreamSocket() {
          throw new Error('not used');
        }
      };
    },
    createMonitoringAdapter() {
      return {
        openSocketMonitor() {
          return {
            nativeInstance: {},
            onEvent() {},
            recv() { return null; },
            status() { return {}; },
            async dispose() {}
          };
        }
      };
    }
  };
}

function fakeReadyBatch() {
  return {
    reset() {},
    takeClaim() { throw new Error('no ready records'); },
    close() {}
  };
}

function fakeReceiveBatch() {
  return {
    reset() {},
    close() {}
  };
}

function fakeConnectableSocket(calls, kind) {
  const socket = {
    nativeInstance: {},
    channelName: '',
    peerWeight: 100,
    sendHighWaterMark: 0,
    receiveHighWaterMark: 0,
    sendTimeoutMs: 0,
    setChannelName(channelName) {
      this.channelName = channelName;
    },
    setRoutingId() {},
    bind(endpoint) {
      calls.push(`${kind}:${this.channelName}:bind:${endpoint}`);
    },
    connect(endpoint) {
      calls.push(`${kind}:${this.channelName}:connect:${endpoint}`);
    },
    disconnect(endpoint) {
      calls.push(`${kind}:${this.channelName}:disconnect:${endpoint}`);
    },
    attachDiscovery() {},
    onSendReady() {},
    send() { return true; },
    request() { return true; },
    recv() { return null; },
    async dispose() {}
  };
  if (kind !== 'router') {
    return socket;
  }
  socket.options = {
    set probe(value) {
      calls.push(`${kind}:${socket.channelName}:probe:${value}`);
    },
    setConnectRoutingId(routingId) {
      calls.push(`${kind}:${socket.channelName}:connectRoutingId:${routingId.toHex()}`);
    }
  };
  return socket;
}

function clientServerDescriptor(owner, channelName, nodeRid, endpoint) {
  return {
    channelName,
    serverRid: nodeRid,
    lifecycleGeneration: 1n,
    descriptorRevision: 1n,
    endpoint,
    weight: 100,
    state: framework.ZLinkFrameworkRuntimeState.Serving,
    securityIdentity: 'default',
    ownerId: owner.ownerId,
    leaseGeneration: owner.leaseGeneration,
    updatedAt: new Date(0)
  };
}

function routePeer(meshName, ownerId, nodeRid, endpoint) {
  return {
    autoConnectType: framework.ZLinkLocationAutoConnectType.RouteMesh,
    meshName,
    nodeRid,
    role: framework.ZLinkLocationRole.Router,
    endpoint,
    weight: 100,
    value: 0n,
    ownerId,
    generation: 0n,
    updatedAt: new Date(0)
  };
}

function fanoutDescriptor(owner, channelName, nodeRid, endpoint) {
  return {
    channelName,
    publisherRid: nodeRid,
    lifecycleGeneration: 1n,
    descriptorRevision: 1n,
    endpoint,
    state: framework.ZLinkFrameworkRuntimeState.Serving,
    securityIdentity: 'default',
    ownerId: owner.ownerId,
    leaseGeneration: owner.leaseGeneration,
    updatedAt: new Date(0)
  };
}

function spotPeer(ownerId, nodeRid, endpoint) {
  return {
    autoConnectType: framework.ZLinkLocationAutoConnectType.RouteMesh,
    meshName: 'play',
    nodeRid,
    role: framework.ZLinkLocationRole.Router,
    endpoint,
    weight: 100,
    value: 0n,
    metadata: {},
    ownerId,
    generation: 0n,
    updatedAt: new Date(0)
  };
}
