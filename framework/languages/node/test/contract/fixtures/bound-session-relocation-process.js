const net = require('node:net');

const framework = require('../../../packages/framework/dist/internal');
const {
  ZLinkActorTransferRuntime
} = require('../../../packages/framework/dist/runtime/host/actor-transfer-runtime');
const serviceWire = require(
  '../../../packages/framework/dist/runtime/foundation/service-stateful-wire-codec'
);
const {
  ZLinkNativeFallbackBoundSession
} = require('../../../packages/framework/dist/runtime/streams/native-fallback-bound-session');
const {
  ZLinkSubmitStatus
} = require('../../../packages/framework/dist/runtime/messaging/submission-result');

const role = requireText(process.env.ZLINK_TEST_ROLE, 'ZLINK_TEST_ROLE');
const actorId = 'actor-two-process-completion';
let server;
let host;
let context;
let writes = 0;
let command42 = 0;
let command44 = 0;
let lastSubmitStatus;
let transferRuntime;
let transferState;
let preparedRelocation;
let authority;

void start().catch(fatal);

async function start() {
  if (role === 'session') await startSessionOwner();
  else if (role === 'target') await startTarget();
  else throw new Error(`Unsupported role '${role}'.`);
  process.on('message', onMessage);
  process.once('SIGTERM', () => void stop());
  send({ type: 'ready', role, pid: process.pid });
}

async function startSessionOwner() {
  host = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration()
  });
  host.routeTransport.requestRawToSpot = async () => [
    require('@zlink-systems/zlink').Message.from(JSON.stringify({
      ok: true,
      response: { acknowledged: true }
    }))
  ];
  context = host.streamBindingRuntime.createSessionContext({
    sessionId: 'session',
    routingId: 'session',
    write() {
      writes++;
      return true;
    },
    writeRaw() {
      writes++;
      return true;
    },
    async submitRaw(message) {
      return {
        status: this.writeRaw(message)
          ? ZLinkSubmitStatus.Submitted
          : ZLinkSubmitStatus.Backpressured
      };
    },
    async close() {}
  });
  await context.actors.bind({
    nodeRid: 'source',
    actorId,
    generation: 5n,
    ownershipGeneration: 11n,
    ownerLeaseGeneration: 13n,
    bindingGeneration: 6n
  });
  const port = Number(requireText(process.env.ZLINK_TEST_PORT, 'ZLINK_TEST_PORT'));
  server = net.createServer(socket => {
    let body = '';
    socket.setEncoding('utf8');
    socket.on('data', chunk => {
      body += chunk;
      if (!body.endsWith('\n')) return;
      void receiveRemote(socket, body.trimEnd());
    });
  });
  await new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(port, '127.0.0.1', resolve);
  });
}

async function startTarget() {
  const port = Number(requireText(process.env.ZLINK_TEST_PORT, 'ZLINK_TEST_PORT'));
  const actor = { context: { actorId, meshName: 'play.route' } };
  const remoteTarget = {
    routerChannelId: 'play.route',
    targetNodeRid: 'session-owner',
    spotId: 'entry',
    sessionNodeRid: 'session-owner',
    sessionRid: 'session',
    bindingGeneration: 6n
  };
  transferState = {
    actorId,
    actorType: 'Player',
    actor,
    meshName: 'play.route',
    nativeActorRef: { actorId, generation: 5n, nodeRid: 'source' },
    locationGeneration: 11n,
    ownerLeaseGeneration: 13n,
    remoteBoundSessionTarget: remoteTarget,
    beginMove() { this.moving = true; },
    endMove() { this.moving = false; },
    setRemoteBoundSessionTarget(value) { this.remoteBoundSessionTarget = value; }
  };
  authority = authoritySnapshot('source', 2n, 'source-owner', 13n, 11n, 'store-v17');
  const sessionRelocationWire = {
    async requestSessionRelocationSeal(_meshName, _targetNodeRid, request) {
      const response = await requestTcp(port, {
        kind: 'command42',
        payload: serviceWire.encodeSessionRelocationSeal(request).toString('base64')
      });
      if (response.ok !== true) throw new Error(response.message ?? 'command 42 failed');
      return serviceWire.decodeSessionRelocationSealed(Buffer.from(response.payload, 'base64'));
    },
    async sendSessionRelocationRoute(_meshName, _targetNodeRid, request) {
      const response = await requestTcp(port, {
        kind: 'command44',
        payload: serviceWire.encodeSessionRelocationRoute(request).toString('base64')
      });
      if (response.ok !== true) throw new Error(response.message ?? 'command 44 failed');
    }
  };
  transferRuntime = new ZLinkActorTransferRuntime({
    authorityStore: () => ({ async readAuthority() { return authority; } }),
    liveDescriptors: async () => [{
      rid: 'session-owner',
      lifecycleGeneration: 4n,
      ownerId: 'session-owner-id',
      leaseGeneration: 8n
    }],
    sessionRelocationWire: () => sessionRelocationWire,
    actorHandoff: {
      begin() {},
      sealConnectionBoundIngress() {},
      snapshot() { return []; },
      takeRelocationRelay() { return []; },
      complete() {}
    },
    actorTransferRegistry: {},
    actorManager: () => ({ getState: () => transferState }),
    spotManager: () => undefined,
    locationLifecycle: () => undefined,
    currentOwner: () => ({ ownerId: authority.ownerId, leaseGeneration: authority.ownerLeaseGeneration }),
    relocationStore: () => undefined,
    routeTransport: {},
    primaryMeshNode: () => ({
      status: () => ({ routingId: 'source', lifecycleGeneration: 2n })
    }),
    notifyEntrySpotActorLeft: async () => {},
    restoreEntrySpotActorJoined: async () => {},
    clearRemoteActorPacketTarget() {}
  });
  const session = new ZLinkNativeFallbackBoundSession({
    runtime: {
      async submitLocalBoundSession() {
        return { status: ZLinkSubmitStatus.TargetNotFound };
      }
    },
    routedTransport: {
      async submitInfrastructure(_channel, _rid, _packetName, payload) {
        const response = await sendBoundSession(port, payload);
        const result = {
          status: response.ok
            ? ZLinkSubmitStatus.Submitted
            : ZLinkSubmitStatus.TargetNotFound
        };
        lastSubmitStatus = result.status;
        return result;
      },
      async sendToSpot() {
        throw new Error('The two-process completion must use node-direct infrastructure transport.');
      }
    },
    actorRefProvider: () => ({
      actorId,
      objectGeneration: 5n,
      meshName: 'play.route',
      nodeRid: 'target',
      ownershipGeneration: 12n,
      bindingGeneration: 6n
    }),
    nativeActorNodeProvider: () => undefined,
    localActorProvider: () => false,
    remoteBoundSessionTargetProvider: () => transferState.remoteBoundSessionTarget,
    remoteActorPacketTargetProvider: () => undefined,
    actorId,
    reportError: error => send({ type: 'send-error', message: String(error) })
  });
  globalThis.targetSession = session;
  globalThis.targetActor = actor;
}

async function receiveRemote(socket, body) {
  try {
    const request = JSON.parse(body);
    if (request.kind === 'command42') {
      command42++;
      const sealed = await host.boundSessionRelay.boundSessions
        .receiveServiceWireSessionRelocationSeal(
          serviceWire.decodeSessionRelocationSeal(Buffer.from(request.payload, 'base64'))
        );
      socket.end(JSON.stringify({
        ok: true,
        payload: serviceWire.encodeSessionRelocationSealed(sealed).toString('base64')
      }));
      return;
    }
    if (request.kind === 'command44') {
      command44++;
      await host.boundSessionRelay.boundSessions.receiveServiceWireSessionRelocationRoute(
        serviceWire.decodeSessionRelocationRoute(Buffer.from(request.payload, 'base64'))
      );
      socket.end(JSON.stringify({ ok: true }));
      return;
    }
    if (request.kind !== 'bound-send') throw new Error(`Unsupported wire kind '${request.kind}'.`);
    const result = await host.boundSessionRelay.boundSessions
      .receiveRemoteBoundSessionSend(request.payload);
    socket.end(JSON.stringify(result));
  } catch (error) {
    socket.end(JSON.stringify({ ok: false, message: error?.stack ?? String(error) }));
  }
}

async function onMessage(message) {
  if (message?.type === 'stop') {
    await stop();
    return;
  }
  if (typeof message?.id !== 'number') return;
  try {
    const value = role === 'session'
      ? await sessionCommand(message.action)
      : await targetCommand(message.action);
    send({ type: 'result', id: message.id, value });
  } catch (error) {
    send({ type: 'error', id: message.id, message: error?.stack ?? String(error) });
  }
}

async function sessionCommand(action) {
  if (action === 'status') return { writes, command42, command44 };
  throw new Error(`Unsupported session action '${action}'.`);
}

async function targetCommand(action) {
  if (action === 'relocate') {
    if (preparedRelocation !== undefined) throw new Error('Relocation already started.');
    preparedRelocation = await transferRuntime.prepareMaintenanceSession(
      globalThis.targetActor,
      transferState,
      undefined,
      false,
      { high: 7n, low: 9n }
    );
    return {
      phase: 'sealed',
      sealId: transferState.remoteBoundSessionTarget.relocationSealId
    };
  }
  if (action === 'sendCompletion') {
    if (preparedRelocation === undefined) throw new Error('Completion cannot precede relocation seal.');
    await globalThis.targetSession
      .send({ completion: 'joined' })
      .packetName('JoinGameNotify')
      .submit();
    return { status: lastSubmitStatus };
  }
  if (action === 'commit') {
    if (preparedRelocation === undefined) throw new Error('Relocation must be sealed before commit.');
    authority = authoritySnapshot('target', 4n, 'target-owner', 14n, 12n, 'store-v18');
    transferState.nativeActorRef = { actorId, generation: 5n, nodeRid: 'target' };
    transferState.locationGeneration = 12n;
    transferState.ownerLeaseGeneration = 14n;
    await preparedRelocation.commit(
      {
        routerChannelId: 'play.route',
        targetNodeRid: 'target',
        spotId: 'entry',
        authorityOwnerGeneration: 12n
      },
      { actorId, objectGeneration: 5n, meshName: 'play.route', nodeRid: 'target' },
      {
        ownerId: 'target-owner',
        ownerLeaseGeneration: 14n,
        nodeRid: 'target',
        nodeGeneration: 4n,
        authorityOwnerGeneration: 12n
      }
    );
    await transferRuntime.publishRoutedActorOwnership(globalThis.targetActor);
    return { phase: 'committed', actorNodeRid: transferState.nativeActorRef.nodeRid };
  }
  throw new Error(`Unsupported target action '${action}'.`);
}

function authoritySnapshot(nodeRid, nodeGeneration, ownerId, ownerLeaseGeneration,
  authorityOwnerGeneration, storeVersion) {
  return {
    kind: 'snapshot',
    storeVersion: { value: storeVersion },
    payload: Buffer.alloc(0),
    objectGeneration: 5n,
    authorityOwnerGeneration,
    ownerId,
    ownerLeaseGeneration,
    allocation: {
      state: 'active',
      objectKind: 'actor',
      stableType: 'Player',
      descriptor: { meshName: 'play.route', rid: nodeRid },
      descriptorLifecycleGeneration: nodeGeneration,
      capacity: { actors: 1, spots: 0 }
    },
    storeNow: new Date()
  };
}

function requestTcp(port, payload) {
  return new Promise((resolve, reject) => {
    const socket = net.createConnection({ host: '127.0.0.1', port }, () => {
      socket.write(`${JSON.stringify(payload)}\n`);
    });
    let body = '';
    socket.setEncoding('utf8');
    socket.on('data', chunk => { body += chunk; });
    socket.on('end', () => resolve(JSON.parse(body)));
    socket.once('error', reject);
  });
}

function sendBoundSession(port, payload) {
  return requestTcp(port, {
    kind: 'bound-send',
    payload
  });
}

async function stop() {
  if (server !== undefined) {
    await new Promise(resolve => server.close(resolve));
  }
  process.exit(0);
}

function fatal(error) {
  send({ type: 'fatal', message: error?.stack ?? String(error) });
  process.exitCode = 1;
}

function send(message) {
  if (typeof process.send === 'function' && process.connected) process.send(message);
}

function requireText(value, name) {
  if (typeof value !== 'string' || value.length === 0) throw new Error(`${name} is required.`);
  return value;
}
