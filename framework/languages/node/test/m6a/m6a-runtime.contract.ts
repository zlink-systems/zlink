import assert from 'node:assert/strict';
import { totalmem } from 'node:os';
import { test } from 'node:test';
import { getHeapStatistics } from 'node:v8';

import {
  SERVICE_WIRE_MAGIC,
  SERVICE_WIRE_MAJOR,
  SERVICE_WIRE_REQUIRED_CAPABILITY,
  ServiceWireCommand
} from '../../../../runtime/protocol/generated/node/service_wire_constants';
import {
  RawServiceMeshRuntime
} from '../../packages/framework/src/runtime/foundation/raw-service-mesh-runtime';
import {
  ZLinkNodeRawMeshBackend
} from '../../packages/framework/src/runtime/backend/node/node-raw-mesh-backend';
import {
  ZLinkInboundDispatchBudget,
  resolveApplicationHwm
} from '../../packages/framework/src/runtime/dispatch/inbound-dispatch-budget';
import {
  ZLinkApplicationHwmProfile
} from '../../packages/framework/src/contracts';
import {
  ServiceDiscoveryRegistry
} from '../../packages/framework/src/runtime/foundation/service-discovery-registry';
import {
  ServiceLivenessRegistry
} from '../../packages/framework/src/runtime/foundation/service-liveness-registry';
import {
  OperationKind,
  operationRequiresReply
} from '../../packages/framework/src/runtime/foundation/service-runtime-contracts';
import {
  ServiceMailbox
} from '../../packages/framework/src/runtime/foundation/service-mailbox';
import {
  ServiceTopologyRegistry,
  type ServiceNodeDescriptor
} from '../../packages/framework/src/runtime/foundation/service-topology-registry';
import {
  M6A_SERVICE_WIRE_MAGIC,
  M6A_SERVICE_WIRE_MAJOR,
  M6A_SERVICE_WIRE_REQUIRED_CAPABILITY,
  M6aServiceWireCommand,
  decodeApplicationPayload,
  decodeRouteMeshAdmission,
  decodeReplyHeader,
  encodeApplicationPayload,
  encodeReplyHeader,
  encodeRouteMeshAdmission
} from '../../packages/framework/src/runtime/foundation/service-wire-m6a-codec';

function descriptor(
  nodeRoutingId: string,
  endpoint = `inproc://m6a-${nodeRoutingId}-${process.pid}`
): ServiceNodeDescriptor {
  return {
    meshName: 'm6a-mesh',
    nodeRoutingId,
    lifecycleGeneration: 1n,
    descriptorRevision: 1n,
    advertisedEndpoint: endpoint,
    channels: [
      { name: 'alpha', weight: 100 },
      { name: 'beta', weight: 50 }
    ],
    state: 'preparing',
    securityIdentity: 'default',
    effectiveMaxMessageBytes: 4 * 1024 * 1024,
    applicationVersion: 1n,
    protocolCapabilities: ['framework-service-v11'],
    objectRole: 'server',
    placementWeight: 100,
    activeCapacityLimit: 10_000,
    pendingCapacityLimit: 128,
    activeCapacityUsed: 0,
    pendingCapacityUsed: 0
  };
}

function runtimeStateWireValue(frame: Uint8Array): number {
  const bytes = Buffer.from(frame);
  let offset = 5;
  assert.equal(bytes[offset++], 1);
  const bodyLength = bytes.readUInt32BE(offset);
  offset += 4;
  const bodyEnd = offset + bodyLength;

  offset += 1 + bytes[offset]!;
  offset += 1 + bytes[offset]!;
  offset += 4 + 8 + 8;
  const endpointLength = bytes.readUInt16BE(offset);
  offset += 2 + endpointLength;
  const channelCount = bytes.readUInt16BE(offset);
  offset += 2;
  for (let index = 0; index < channelCount; index++) {
    offset += 1 + bytes[offset]! + 4;
  }

  const extensionLength = bytes.readUInt32BE(offset);
  offset += 4;
  assert.equal(offset + extensionLength, bodyEnd);
  const extensionEnd = offset + extensionLength;
  while (offset < extensionEnd) {
    const id = bytes[offset++];
    const length = bytes.readUInt32BE(offset);
    offset += 4;
    if (id === 1) {
      assert.equal(length, 1);
      return bytes[offset]!;
    }
    offset += length;
  }
  assert.fail('runtime-state descriptor field was not encoded');
}

test('M6A runtime command subset matches the generated wire schema', () => {
  assert.deepEqual(M6A_SERVICE_WIRE_MAGIC, SERVICE_WIRE_MAGIC);
  assert.equal(M6A_SERVICE_WIRE_MAJOR, SERVICE_WIRE_MAJOR);
  assert.equal(M6A_SERVICE_WIRE_REQUIRED_CAPABILITY, SERVICE_WIRE_REQUIRED_CAPABILITY);
  for (const name of Object.keys(M6aServiceWireCommand) as Array<keyof typeof M6aServiceWireCommand>) {
    assert.equal(M6aServiceWireCommand[name], ServiceWireCommand[name]);
  }
});

test('RouteMesh runtime state uses the shared service wire values', () => {
  const expected = [
    ['preparing', 0],
    ['serving', 1],
    ['draining', 2],
    ['stopped', 3],
    ['error', 4]
  ] as const;

  for (const [state, wireValue] of expected) {
    const frame = encodeRouteMeshAdmission(M6aServiceWireCommand.update, {
      ...descriptor(`state-${state}`),
      state
    });
    assert.equal(runtimeStateWireValue(frame), wireValue);
    assert.equal(
      decodeRouteMeshAdmission(frame, M6aServiceWireCommand.update, `state-${state}`).state,
      state
    );
  }

  const retiring = encodeRouteMeshAdmission(M6aServiceWireCommand.update, {
    ...descriptor('state-retiring'),
    state: 'retiring'
  });
  assert.equal(runtimeStateWireValue(retiring), 2);
  assert.equal(
    decodeRouteMeshAdmission(
      retiring,
      M6aServiceWireCommand.update,
      'state-retiring'
    ).state,
    'draining'
  );
});

test('reply header preserves the schema tail length field', () => {
  const empty = encodeReplyHeader(7n);
  assert.equal(empty.byteLength, 23);
  assert.deepEqual(decodeReplyHeader(empty), {
    correlation: 7n,
    terminalResult: 0,
    failureCode: 0,
    tail: Buffer.alloc(0)
  });

  const tail = encodeReplyHeader(8n, 102, 17, Uint8Array.from([1, 2, 3]));
  assert.deepEqual(decodeReplyHeader(tail), {
    correlation: 8n,
    terminalResult: 102,
    failureCode: 17,
    tail: Buffer.from([1, 2, 3])
  });
});

test('application payload encoding writes an offset Uint8Array into one owned frame', () => {
  const source = Uint8Array.from([0, 1, 2, 3, 4, 5]);
  const frame = encodeApplicationPayload({
    packetName: 'Notice',
    contentType: 'application/octet-stream',
    payload: source.subarray(2, 5)
  });
  source.fill(9);

  assert.deepEqual(decodeApplicationPayload(frame), {
    packetName: 'Notice',
    contentType: 'application/octet-stream',
    payload: Buffer.from([2, 3, 4])
  });
});

test('topology snapshots fence reconnect and exclude retiring placement targets', () => {
  const topology = new ServiceTopologyRegistry(descriptor('local'));
  const peer = { ...descriptor('peer'), state: 'serving' as const };
  assert.equal(topology.admit(peer, 'connection-a'), 'admitted');
  assert.equal(topology.selectChannel('alpha')?.descriptor.nodeRoutingId, 'peer');
  assert.equal(topology.selectPlacement()?.descriptor.nodeRoutingId, 'peer');

  assert.equal(topology.admit(peer, 'connection-b'), 'staleDescriptor');
  assert.equal(topology.disconnect('peer', 'connection-b'), false);
  assert.equal(topology.peer('peer')?.connectionId, 'connection-a');

  const conflicting = { ...peer, state: 'retiring' as const };
  assert.equal(topology.admit(conflicting, 'connection-b'), 'staleDescriptor');
  const retiring = { ...conflicting, descriptorRevision: 2n };
  assert.equal(topology.admit(retiring, 'connection-b'), 'admitted');
  assert.equal(topology.disconnect('peer', 'connection-a'), false);
  assert.equal(topology.selectChannel('alpha'), undefined);
  assert.equal(topology.selectPlacement(), undefined);
});

test('channel selection excludes admitted peers until the caller confirms readiness', () => {
  const topology = new ServiceTopologyRegistry({
    ...descriptor('local'),
    state: 'serving',
    channels: [{ name: 'alpha', weight: 0 }]
  });
  const peer = { ...descriptor('peer'), state: 'serving' as const };
  assert.equal(topology.admit(peer, 'connection-a'), 'admitted');

  assert.equal(topology.selectChannel('alpha', () => false), undefined);
  assert.equal(
    topology.selectChannel('alpha', candidate => candidate.descriptor.nodeRoutingId === 'peer')
      ?.descriptor.nodeRoutingId,
    'peer'
  );
});

test('object placement excludes admitted peers until the caller confirms readiness', () => {
  const topology = new ServiceTopologyRegistry({
    ...descriptor('local'),
    state: 'serving',
    objectRole: 'client',
    channels: [{ name: 'alpha', weight: 0 }]
  });
  const peer = {
    ...descriptor('peer'),
    state: 'serving' as const,
    protocolCapabilities: ['framework-service-v11', 'object-type:quest']
  };
  assert.equal(topology.admit(peer, 'connection-a'), 'admitted');
  assert.equal(topology.selectObjectPlacement('quest', () => false), undefined);
  assert.equal(
    topology.selectObjectPlacement('quest', candidate => candidate.nodeRoutingId === 'peer')
      ?.nodeRoutingId,
    'peer'
  );
});

test('object placement checks capacity before applying a zero placement weight', () => {
  const exhausted = new ServiceTopologyRegistry({
    ...descriptor('zero-weight-exhausted'),
    state: 'serving',
    placementWeight: 0,
    protocolCapabilities: ['framework-service-v11', 'object-type:quest'],
    activeCapacityLimit: 1,
    activeCapacityUsed: 1
  });
  assert.equal(exhausted.objectPlacementStatus('quest'), 'capacity');

  const filtered = new ServiceTopologyRegistry({
    ...descriptor('zero-weight-available'),
    state: 'serving',
    placementWeight: 0,
    protocolCapabilities: ['framework-service-v11', 'object-type:quest']
  });
  assert.equal(filtered.objectPlacementStatus('quest'), 'unsupported');
});

test('topology admission fences expected identity, immutable revisions, duplicate pipes, and late disconnect', () => {
  const topology = new ServiceTopologyRegistry(descriptor('local'));
  const peer = { ...descriptor('peer'), state: 'serving' as const };
  assert.equal(topology.admit(peer, 'pipe-z', {
    endpoint: peer.advertisedEndpoint,
    securityIdentity: peer.securityIdentity,
    lifecycleGeneration: peer.lifecycleGeneration
  }), 'admitted');
  assert.equal(topology.admit(peer, 'pipe-z', {
    endpoint: peer.advertisedEndpoint,
    securityIdentity: peer.securityIdentity,
    lifecycleGeneration: peer.lifecycleGeneration
  }), 'admitted');

  assert.equal(topology.admit(
    { ...peer, advertisedEndpoint: 'inproc://unexpected' },
    'pipe-y',
    { endpoint: peer.advertisedEndpoint, securityIdentity: peer.securityIdentity }
  ), 'invalidDescriptor');
  assert.equal(topology.admit(
    { ...peer, lifecycleGeneration: 2n },
    'pipe-y',
    {
      endpoint: peer.advertisedEndpoint,
      securityIdentity: peer.securityIdentity,
      lifecycleGeneration: peer.lifecycleGeneration
    }
  ), 'invalidDescriptor');
  assert.equal(topology.admit(
    { ...peer, securityIdentity: 'other' },
    'pipe-y',
    { endpoint: peer.advertisedEndpoint, securityIdentity: peer.securityIdentity }
  ), 'invalidDescriptor');

  assert.equal(topology.admit({
    ...peer,
    descriptorRevision: 2n,
    channels: [{ name: 'different', weight: 100 }]
  }, 'pipe-z'), 'invalidDescriptor');
  assert.equal(topology.admit({
    ...peer,
    descriptorRevision: 2n,
    objectRole: 'client'
  }, 'pipe-z'), 'invalidDescriptor');
  const immutableRevisions: ServiceNodeDescriptor[] = [
    {
      ...peer,
      descriptorRevision: 2n,
      effectiveMaxMessageBytes: peer.effectiveMaxMessageBytes + 1
    },
    {
      ...peer,
      descriptorRevision: 2n,
      applicationVersion: peer.applicationVersion + 1n
    },
    {
      ...peer,
      descriptorRevision: 2n,
      protocolCapabilities: ['framework-service-v11', 'object-type:other']
    },
    {
      ...peer,
      descriptorRevision: 2n,
      activeCapacityLimit: peer.activeCapacityLimit + 1
    },
    {
      ...peer,
      descriptorRevision: 2n,
      pendingCapacityLimit: peer.pendingCapacityLimit + 1
    }
  ];
  for (const revision of immutableRevisions) {
    assert.equal(topology.admit(revision, 'pipe-z'), 'invalidDescriptor');
  }

  // Both endpoints rank the connection initiated by the smaller RID first.
  assert.equal(topology.admit(peer, 'pipe-a', undefined, 'initiator:local'), 'admitted');
  assert.equal(topology.peer('peer')?.connectionId, 'pipe-a');
  assert.equal(
    topology.admit(peer, 'pipe-0', undefined, 'initiator:peer'),
    'staleDescriptor'
  );
  assert.equal(topology.disconnect('peer', 'pipe-z'), false);
  assert.equal(topology.peer('peer')?.connectionId, 'pipe-a');
});

test('topology treats lifecycle generation as an opaque equality token', () => {
  const topology = new ServiceTopologyRegistry(descriptor('local'));
  const first = {
    ...descriptor('peer'),
    lifecycleGeneration: 99n,
    state: 'serving' as const
  };
  const replacement = {
    ...first,
    lifecycleGeneration: 3n,
    descriptorRevision: 1n
  };

  assert.equal(topology.admit(first, 'old-connection'), 'admitted');
  assert.equal(topology.admit(replacement, 'current-connection'), 'admitted');
  assert.equal(topology.peer('peer')?.descriptor.lifecycleGeneration, 3n);
  assert.equal(topology.disconnect('peer', 'old-connection'), false);
  assert.equal(topology.peer('peer')?.connectionId, 'current-connection');
});

test('raw monitor preserves each physical candidate direction through admission and disconnect fencing', () => {
  const runtime = new RawServiceMeshRuntime({ descriptor: descriptor('local') });
  const peer = { ...descriptor('peer'), state: 'serving' as const };
  const internal = runtime as unknown as {
    expectedPeers: Map<string, {
      meshName: string;
      nodeRoutingId: string;
      endpoint: string;
      securityIdentity: string;
    }>;
    connectionCandidates: Map<string, Map<string, {
      connectionId: string;
      direction: string;
      discriminator: string;
    }>>;
    connectionIds: Map<string, string>;
    monitorEvents: Array<{
      event: number;
      value: number;
      routingId: string;
      localAddress: string;
      remoteAddress: string;
    }>;
  };
  internal.expectedPeers.set('peer', {
    meshName: peer.meshName,
    nodeRoutingId: peer.nodeRoutingId,
    endpoint: peer.advertisedEndpoint,
    securityIdentity: peer.securityIdentity
  });
  internal.monitorEvents.push(
    {
      event: 0x1000,
      value: 11,
      routingId: 'peer',
      localAddress: 'tcp://ephemeral:41001',
      remoteAddress: peer.advertisedEndpoint
    },
    {
      event: 0x1000,
      value: 22,
      routingId: 'peer',
      localAddress: runtime.topology.localDescriptor().advertisedEndpoint,
      remoteAddress: 'tcp://peer-ephemeral:42001'
    }
  );

  assert.equal(runtime.drainMonitorEvents(), 2);
  const candidates = [...internal.connectionCandidates.get('peer')!.values()];
  assert.deepEqual(
    candidates.map(candidate => [candidate.direction, candidate.discriminator]),
    [
      ['outbound', 'initiator:local'],
      ['inbound', 'initiator:peer']
    ]
  );
  const inbound = candidates[1]!;
  assert.equal(
    runtime.topology.admit(
      peer,
      inbound.connectionId,
      undefined,
      inbound.discriminator
    ),
    'admitted'
  );

  internal.monitorEvents.push({
    event: 0x0200,
    value: 11,
    routingId: 'peer',
    localAddress: 'tcp://ephemeral:41001',
    remoteAddress: peer.advertisedEndpoint
  });
  assert.equal(runtime.drainMonitorEvents(), 1);
  assert.equal(runtime.topology.peer('peer')?.connectionId, inbound.connectionId);
  assert.equal(internal.connectionCandidates.get('peer')?.size, 1);
  assert.equal(internal.connectionIds.get('peer'), inbound.connectionId);
});

test('raw monitor ignores a late disconnect from the superseded physical connection', () => {
  const runtime = new RawServiceMeshRuntime({ descriptor: descriptor('local') });
  const peer = { ...descriptor('peer'), state: 'serving' as const };
  const internal = runtime as unknown as {
    connectionIds: Map<string, string>;
    monitorEvents: Array<{
      event: number;
      value: number;
      routingId: string;
      localAddress: string;
      remoteAddress: string;
    }>;
  };
  const currentConnection = JSON.stringify([22, 'local', 'remote']);
  internal.connectionIds.set('peer', currentConnection);
  assert.equal(runtime.topology.admit(peer, currentConnection), 'admitted');
  internal.monitorEvents.push({
    event: 0x0200,
    value: 11,
    routingId: 'peer',
    localAddress: 'local',
    remoteAddress: 'remote'
  });

  assert.equal(runtime.drainMonitorEvents(), 1);
  assert.equal(runtime.topology.peer('peer')?.connectionId, currentConnection);
  assert.equal(internal.connectionIds.get('peer'), currentConnection);
});

test('raw disconnect fences a late lifecycle generation after peer replacement', () => {
  const endpoint = `ipc:///tmp/zlink-m6a-generation-fence-${process.pid}-${Date.now()}.sock`;
  const runtime = new RawServiceMeshRuntime({
    descriptor: descriptor('local-generation-fence', endpoint)
  });
  runtime.start();
  try {
    const peerEndpoint = 'tcp://peer-generation-fence:7001';
    const current = {
      ...descriptor('peer-generation-fence', peerEndpoint),
      lifecycleGeneration: 3n,
      state: 'serving' as const
    };
    assert.equal(runtime.topology.admit(current, 'current-connection'), 'admitted');

    runtime.disconnectPeer(peerEndpoint, current.nodeRoutingId, 99n);

    assert.equal(
      runtime.topology.peer(current.nodeRoutingId)?.descriptor.lifecycleGeneration,
      3n
    );
    assert.equal(
      runtime.topology.peer(current.nodeRoutingId)?.connectionId,
      'current-connection'
    );
  } finally {
    runtime.close();
  }
});

test('raw disconnect tolerates a late native route removal after the peer is already gone', () => {
  const endpoint = `ipc:///tmp/zlink-m6a-late-disconnect-${process.pid}-${Date.now()}.sock`;
  const runtime = new RawServiceMeshRuntime({
    descriptor: descriptor('local-late-disconnect', endpoint)
  });
  runtime.start();
  try {
    const peer = {
      ...descriptor('peer-late-disconnect', 'tcp://peer-late-disconnect:7001'),
      state: 'serving' as const
    };
    assert.equal(runtime.topology.admit(peer, 'late-connection'), 'admitted');
    assert.doesNotThrow(() => {
      runtime.disconnectPeer(peer.advertisedEndpoint, peer.nodeRoutingId, peer.lifecycleGeneration);
    });
    assert.equal(runtime.topology.peer(peer.nodeRoutingId), undefined);
  } finally {
    runtime.close();
  }
});

test('Object Client pairs are NotRequired only when neither side has a RouteMesh server channel', () => {
  const client = {
    ...descriptor('local-client'),
    objectRole: 'client' as const,
    channels: [],
    state: 'serving' as const
  };
  const remoteClient = {
    ...client,
    nodeRoutingId: 'remote-client',
    advertisedEndpoint: 'inproc://remote-client'
  };
  const topology = new ServiceTopologyRegistry(client);

  assert.equal(topology.admit(remoteClient, 'client-pair'), 'notRequired');
  assert.equal(topology.peers().length, 0);
  assert.equal(topology.notRequiredPeers()[0]?.nodeRoutingId, 'remote-client');

  const weightZeroServerMembership = {
    ...remoteClient,
    descriptorRevision: 2n,
    channels: [{ name: 'control', weight: 0 }]
  };
  assert.equal(
    topology.admit(weightZeroServerMembership, 'weight-zero-server'),
    'admitted'
  );
  assert.equal(topology.notRequiredPeers().length, 0);
  assert.equal(topology.peer('remote-client')?.connectionId, 'weight-zero-server');

  const remoteServer = {
    ...remoteClient,
    nodeRoutingId: 'remote-server',
    advertisedEndpoint: 'inproc://remote-server',
    objectRole: 'server' as const
  };
  assert.equal(topology.admit(remoteServer, 'object-server'), 'admitted');
});

test('public weights preserve boundaries, descriptor revisions, ratios, and capacity-first selection', () => {
  const topology = new ServiceTopologyRegistry({
    ...descriptor('local'),
    state: 'serving',
    placementWeight: 0,
    channels: [{ name: 'alpha', weight: 0 }]
  });
  const low = {
    ...descriptor('low'),
    state: 'serving' as const,
    channels: [{ name: 'alpha', weight: 100 }],
    placementWeight: 100
  };
  const high = {
    ...descriptor('high'),
    state: 'serving' as const,
    channels: [{ name: 'alpha', weight: 300 }],
    placementWeight: 300
  };
  const full = {
    ...descriptor('full'),
    state: 'serving' as const,
    channels: [{ name: 'alpha', weight: 0 }],
    placementWeight: 10_000,
    activeCapacityUsed: 10_000
  };
  assert.equal(topology.admit(low, 'low-1'), 'admitted');
  assert.equal(topology.admit(high, 'high-1'), 'admitted');
  assert.equal(topology.admit(full, 'full-1'), 'admitted');

  const channelCounts = new Map<string, number>();
  const placementCounts = new Map<string, number>();
  for (let index = 0; index < 400; index++) {
    const channel = topology.selectChannel('alpha')!.descriptor.nodeRoutingId;
    channelCounts.set(channel, (channelCounts.get(channel) ?? 0) + 1);
    const placement = topology.selectPlacement()!.descriptor.nodeRoutingId;
    placementCounts.set(placement, (placementCounts.get(placement) ?? 0) + 1);
  }
  assert.deepEqual(Object.fromEntries(channelCounts), { high: 300, low: 100 });
  assert.deepEqual(Object.fromEntries(placementCounts), { high: 300, low: 100 });
  assert.equal(channelCounts.has('full'), false);
  assert.equal(placementCounts.has('full'), false);

  const balanced = new ServiceTopologyRegistry({
    ...descriptor('balanced-local'),
    state: 'serving',
    channels: [{ name: 'alpha', weight: 0 }]
  });
  assert.equal(balanced.admit({
    ...descriptor('balanced-a'),
    state: 'serving',
    channels: [{ name: 'alpha', weight: 100 }]
  }, 'balanced-a-1'), 'admitted');
  assert.equal(balanced.admit({
    ...descriptor('balanced-b'),
    state: 'serving',
    channels: [{ name: 'alpha', weight: 100 }]
  }, 'balanced-b-1'), 'admitted');
  const balancedCounts = new Map<string, number>();
  for (let index = 0; index < 40; index += 1) {
    const selected = balanced.selectChannel('alpha')!.descriptor.nodeRoutingId;
    balancedCounts.set(selected, (balancedCounts.get(selected) ?? 0) + 1);
  }
  assert.deepEqual(Object.fromEntries(balancedCounts), { 'balanced-a': 20, 'balanced-b': 20 });

  const disabledHigh = {
    ...high,
    descriptorRevision: 2n,
    channels: [{ name: 'alpha', weight: 0 }],
    placementWeight: 0
  };
  assert.equal(topology.admit(disabledHigh, 'high-1'), 'admitted');
  assert.equal(topology.selectChannel('alpha')?.descriptor.nodeRoutingId, 'low');
  assert.equal(topology.selectPlacement()?.descriptor.nodeRoutingId, 'low');
  assert.throws(
    () => topology.publishLocal({
      ...topology.localDescriptor(),
      descriptorRevision: 2n,
      placementWeight: -1
    }),
    /0\.\.10000/
  );
  assert.throws(
    () => topology.publishLocal({
      ...topology.localDescriptor(),
      descriptorRevision: 2n,
      channels: [{ name: 'alpha', weight: 10_001 }]
    }),
    /0\.\.10000/
  );
});

test('ClientServer selection uses overflow-safe weights and excludes zero-weight revisions', () => {
  const discovery = new ServiceDiscoveryRegistry();
  const add = (serverRoutingId: string, weight: number, descriptorRevision = 1n) =>
    discovery.admitClientServer({
      channelName: 'orders',
      serverRoutingId,
      lifecycleGeneration: 1n,
      descriptorRevision,
      weight,
      state: 'serving',
      securityIdentity: 'default',
      effectiveMaxMessageBytes: 1024,
      advertisedEndpoint: `tcp://${serverRoutingId}:7001`
    }, `${serverRoutingId}-${descriptorRevision}`);
  assert.equal(add('low', 100), true);
  assert.equal(add('high', 300), true);

  const counts = new Map<string, number>();
  for (let index = 0; index < 400; index++) {
    const selected = discovery.selectClientServer('orders')!.serverRoutingId;
    counts.set(selected, (counts.get(selected) ?? 0) + 1);
  }
  assert.deepEqual(Object.fromEntries(counts), { high: 300, low: 100 });
  assert.equal(add('high', 0, 2n), true);
  assert.equal(discovery.selectClientServer('orders')?.serverRoutingId, 'low');
  assert.throws(() => add('invalid-negative', -1), /0\.\.10000/);
  assert.throws(() => add('invalid-high', 10_001), /0\.\.10000/);
});

test('ClientServer keeps a known target observable while its transport is disconnected', () => {
  const discovery = new ServiceDiscoveryRegistry();
  const descriptor = {
    channelName: 'orders',
    serverRoutingId: 'server-a',
    lifecycleGeneration: 1n,
    descriptorRevision: 1n,
    weight: 100,
    state: 'serving' as const,
    securityIdentity: 'default',
    effectiveMaxMessageBytes: 1024,
    advertisedEndpoint: 'tcp://server-a:7001'
  };

  assert.equal(discovery.admitClientServer(descriptor, 'connection-a'), true);
  assert.equal(
    discovery.markClientServerDisconnected('orders', 'server-a', 'connection-a'),
    true
  );
  assert.equal(discovery.selectClientServer('orders'), undefined);
  assert.equal(discovery.clientServerDescriptors('orders')[0]?.state, 'disconnected');

  assert.equal(
    discovery.admitClientServer({ ...descriptor, state: 'serving' }, 'connection-b'),
    true
  );
  assert.equal(discovery.selectClientServer('orders')?.serverRoutingId, 'server-a');
  assert.equal(
    discovery.markClientServerDisconnected('orders', 'server-a', 'connection-a'),
    false
  );
});

test('runtime weight changes increment the local descriptor revision and preserve public bounds', () => {
  const runtime = new RawServiceMeshRuntime({
    descriptor: {
      ...descriptor('runtime-options'),
      state: 'serving'
    }
  });
  const initial = runtime.topology.localDescriptor();
  runtime.updateLocalWeights({ placementWeight: 0 });
  const placement = runtime.topology.localDescriptor();
  assert.equal(placement.descriptorRevision, initial.descriptorRevision + 1n);
  assert.equal(placement.placementWeight, 0);

  runtime.updateLocalWeights({
    channelName: 'alpha',
    channelWeight: 10_000
  });
  const channel = runtime.topology.localDescriptor();
  assert.equal(channel.descriptorRevision, placement.descriptorRevision + 1n);
  assert.equal(channel.channels.find(candidate => candidate.name === 'alpha')?.weight, 10_000);
});

test('mailbox domains remain bounded and infrastructure claims progress independently', () => {
  const mailbox = new ServiceMailbox({
    applicationMessages: 2,
    applicationBytes: 8,
    infrastructureMessages: 1,
    infrastructureBytes: 8
  });
  assert.equal(mailbox.tryEnqueue({
    owner: 'spot-a',
    domain: 'application',
    parts: [Buffer.from([1, 2, 3])]
  }), true);
  assert.equal(mailbox.tryEnqueue({
    owner: 'spot-a',
    domain: 'application',
    parts: [Buffer.from([4, 5])]
  }), true);
  assert.equal(mailbox.tryEnqueue({
    owner: 'spot-b',
    domain: 'application',
    parts: [Buffer.from([6])]
  }), false);
  assert.equal(mailbox.tryEnqueue({
    owner: 'peer-a',
    domain: 'infrastructure',
    parts: [Buffer.from([9])]
  }), true);

  const application = mailbox.tryClaim('application', 1, 8)!;
  assert.equal(mailbox.tryClaim('application', 1, 8), undefined);
  const infrastructure = mailbox.tryClaim('infrastructure', 1, 8)!;
  assert.equal(infrastructure.records.length, 1);
  assert.equal(mailbox.release(infrastructure), true);
  assert.equal(mailbox.release(infrastructure), false);
  assert.equal(mailbox.release(application), true);
  assert.equal(mailbox.tryClaim('application', 1, 8)?.records.length, 1);
});

test('liveness uses 5s/15s defaults, reuses outstanding probes, and fences old connections', () => {
  const liveness = new ServiceLivenessRegistry();
  liveness.admit('peer', 'connection-a', 0);
  assert.equal(liveness.isReady('peer', 'connection-a'), false);
  const first = liveness.tick(5_000);
  assert.equal(first.probes.length, 1);
  const probeId = first.probes[0]!.probeId;
  assert.equal(liveness.tick(10_000).probes[0]!.probeId, probeId);
  assert.equal(liveness.acknowledge('peer', 'connection-a', probeId, 10_001), true);
  assert.equal(liveness.isReady('peer', 'connection-a'), true);

  liveness.admit('peer', 'connection-b', 10_002);
  assert.equal(liveness.disconnect('peer', 'connection-a'), false);
  assert.equal(liveness.acknowledge('peer', 'connection-a', probeId, 10_003), false);
  assert.deepEqual(liveness.tick(25_002).timedOutNodes, ['peer']);
});

test('ClientServer selection and classic fanout discovery use dedicated descriptor sets', () => {
  const discovery = new ServiceDiscoveryRegistry();
  assert.equal(discovery.admitClientServer({
    channelName: 'orders',
    serverRoutingId: 'server-a',
    lifecycleGeneration: 1n,
    descriptorRevision: 1n,
    weight: 100,
    state: 'serving',
    securityIdentity: 'default',
    effectiveMaxMessageBytes: 1024,
    advertisedEndpoint: 'tcp://server-a:7001'
  }, 'connection-a'), true);
  assert.equal(discovery.selectClientServer('orders')?.serverRoutingId, 'server-a');
  assert.equal(discovery.removeClientServer('orders', 'server-a', 'old-connection'), false);
  assert.equal(discovery.admitClientServer({
    channelName: 'orders',
    serverRoutingId: 'server-a',
    lifecycleGeneration: 1n,
    descriptorRevision: 1n,
    weight: 100,
    state: 'serving',
    securityIdentity: 'default',
    effectiveMaxMessageBytes: 1024,
    advertisedEndpoint: 'tcp://server-a:7001'
  }, 'connection-b'), true);
  assert.equal(discovery.removeClientServer('orders', 'server-a', 'connection-a'), false);

  assert.equal(discovery.admitFanoutPublisher({
    channelName: 'events',
    publisherRoutingId: 'publisher-a',
    lifecycleGeneration: 1n,
    descriptorRevision: 1n,
    advertisedEndpoint: 'tcp://publisher-a:7002',
    state: 'serving'
  }, 'fanout-a'), true);
  assert.deepEqual(
    discovery.fanoutEndpoints('events').map(value => value.publisherRoutingId),
    ['publisher-a']
  );
});

test('raw admission keeps Object Client-only pairs out of liveness and records NotRequired', async () => {
  const nonce = `${process.pid}-${Date.now()}`;
  const leftDescriptor = {
    ...descriptor(
      'client-left',
      `ipc:///tmp/zlink-m6a-client-left-${nonce}.sock`
    ),
    channels: [],
    objectRole: 'client' as const
  };
  const rightDescriptor = {
    ...descriptor(
      'client-right',
      `ipc:///tmp/zlink-m6a-client-right-${nonce}.sock`
    ),
    channels: [],
    objectRole: 'client' as const
  };
  const left = new RawServiceMeshRuntime({ descriptor: leftDescriptor });
  const right = new RawServiceMeshRuntime({ descriptor: rightDescriptor });
  left.start();
  right.start();
  try {
    left.connectPeer(rightDescriptor.advertisedEndpoint, rightDescriptor);
    await pollUntil(() => {
      left.announceExpectedPeers();
      right.pumpOne();
      left.pumpOne();
      return left.topology.notRequiredPeers().length === 1
        && right.topology.notRequiredPeers().length === 1;
    });

    assert.equal(left.topology.peers().length, 0);
    assert.equal(right.topology.peers().length, 0);
    assert.equal(left.liveness.size, 0);
    assert.equal(right.liveness.size, 0);
    assert.equal(
      left.isObjectClientNodeDirectTarget('client-right'),
      true
    );
  } finally {
    left.close();
    right.close();
  }

  const backend = new ZLinkNodeRawMeshBackend(
    'm6a-mesh',
    'client-monitor'
  );
  backend.configureObjectPlacement({
    role: 'client',
    placementWeight: 100,
    activeCapacityLimit: 10_000,
    pendingCapacityLimit: 128,
    objectCapabilities: []
  });
  backend.setBind(
    `ipc:///tmp/zlink-m6a-client-monitor-${process.pid}-${Date.now()}.sock`
  );
  backend.start();
  try {
    backend.replaceDiscoveredNotRequiredPeers([{
      nodeRoutingId: 'client-peer',
      lifecycleGeneration: 7n,
      descriptorRevision: 9n,
      endpoint: 'tcp://client-peer'
    }]);
    assert.equal(backend.status().admittedPeerCount, 0);
    assert.deepEqual(
      backend.peers().map(peer => ({
        rid: String(peer.routingId),
        state: peer.state,
        lifecycleGeneration: peer.lifecycleGeneration
      })),
      [{
        rid: 'client-peer',
        state: 6,
        lifecycleGeneration: 7n
      }]
    );
  } finally {
    backend.close();
  }
});

test('raw runtime admits peers and completes node/channel requests once', async () => {
  const endpointNonce = `${process.pid}-${Date.now()}`;
  const leftDescriptor = {
    ...descriptor('m6a-left', `ipc:///tmp/zlink-m6a-left-${endpointNonce}.sock`),
    channels: [
      { name: 'alpha', weight: 0 },
      { name: 'beta', weight: 50 }
    ]
  };
  const rightDescriptor = descriptor('m6a-right', `ipc:///tmp/zlink-m6a-right-${endpointNonce}.sock`);
  const left = new RawServiceMeshRuntime({ descriptor: leftDescriptor });
  const right = new RawServiceMeshRuntime({ descriptor: rightDescriptor });
  left.start();
  right.start();
  try {
    left.connectPeer(rightDescriptor.advertisedEndpoint, rightDescriptor);
    await pollUntil(() => {
      left.announceExpectedPeers();
      right.pumpOne();
      left.pumpOne();
      left.tickLiveness();
      right.tickLiveness();
      return left.topology.peer('m6a-right') !== undefined
        && right.topology.peer('m6a-left') !== undefined
        && left.isPeerRouteReady('m6a-right')
        && right.isPeerRouteReady('m6a-left');
    });
    assert.equal(left.isPeerRouteReady('m6a-right', rightDescriptor.lifecycleGeneration), true);
    assert.equal(left.isPeerRouteReady('m6a-right', rightDescriptor.lifecycleGeneration + 1n), false);

    assert.equal(left.sendToChannel('alpha', {
      packetName: 'ChannelNotice',
      contentType: 'application/json',
      payload: Buffer.from('notice')
    }), true);
    await pollUntil(() => right.pumpOne() === 'application');
    const sent = right.mailbox.tryClaim('application', 1, 4096)!;
    assert.equal(sent.owner, 'channel:alpha');
    assert.equal(right.mailbox.release(sent), true);

    const pending = left.requestToNode('m6a-right', {
      packetName: 'Question',
      contentType: 'application/json',
      payload: Buffer.from('request')
    }, 2_000);
    await pollUntil(() => right.pumpOne() === 'application');
    const request = right.mailbox.tryClaim('application', 1, 4096)!;
    right.reply(request.records[0]!, {
      packetName: 'Answer',
      contentType: 'application/json',
      payload: Buffer.from('reply')
    });
    assert.equal(right.mailbox.release(request), true);
    const result = await awaitWithin(
      pending.promise,
      2_000,
      'Timed out waiting for the raw request reply.'
    );
    assert.equal(result.terminalResult, 0);
    assert.equal(Buffer.from(result.payload!.payload).toString(), 'reply');
  } finally {
    left.close();
    right.close();
  }

  const backend = new ZLinkNodeRawMeshBackend('m6a-mesh', 'backend-host');
  backend.setBind(`ipc:///tmp/zlink-m6a-backend-${process.pid}-${Date.now()}.sock`);
  backend.addChannelName('alpha');
  backend.start();
  try {
    assert.equal(backend.status().state, 2);
    const initialRevision = backend.status().descriptorRevision;
    backend.setPlacementWeight(0);
    assert.equal(backend.status().descriptorRevision, initialRevision + 1n);
    backend.setChannelWeight('alpha', 0);
    assert.equal(backend.status().descriptorRevision, initialRevision + 2n);
    assert.throws(() => backend.setPlacementWeight(-1), /0\.\.10000/);
    assert.throws(() => backend.setChannelWeight('alpha', 10_001), /0\.\.10000/);
    const publisher = backend.createPublisher();
    await publisher.publishAsync(
      'alpha',
      'topic',
      [Buffer.from('event')]
    );
    publisher.close();
  } finally {
    backend.close();
  }
});

test('completion control preserves liveness while Application receive is paused', async () => {
  const endpointNonce = `${process.pid}-${Date.now()}`;
  const leftDescriptor = descriptor(
    'completion-left',
    `ipc:///tmp/zlink-m6a-completion-left-${endpointNonce}.sock`
  );
  const rightDescriptor = descriptor(
    'completion-right',
    `ipc:///tmp/zlink-m6a-completion-right-${endpointNonce}.sock`
  );
  const left = new RawServiceMeshRuntime({
    descriptor: leftDescriptor,
    probeIntervalMs: 1,
    peerTimeoutMs: 5_000
  });
  const right = new RawServiceMeshRuntime({
    descriptor: rightDescriptor,
    probeIntervalMs: 1,
    peerTimeoutMs: 5_000
  });
  left.start();
  right.start();
  try {
    left.connectPeer(rightDescriptor.advertisedEndpoint, rightDescriptor);
    await pollUntil(() => {
      left.announceExpectedPeers();
      right.pumpOne();
      left.pumpOne();
      return left.topology.peer(rightDescriptor.nodeRoutingId) !== undefined
        && right.topology.peer(leftDescriptor.nodeRoutingId) !== undefined;
    });

    assert.equal(left.sendToNode(rightDescriptor.nodeRoutingId, {
      packetName: 'ApplicationMustRemainUnread',
      contentType: 'application/octet-stream',
      payload: Buffer.from('unread')
    }), true);

    // Object traffic cannot be smuggled onto Completion, and a control record
    // cannot consume the connection's entire 256 KiB HWM.
    assert.equal(left.sendCompletionControl(
      rightDescriptor.nodeRoutingId,
      [Buffer.from([0x5a, 0x4d, 1, M6aServiceWireCommand.nodeSend, 0])]
    ), false);
    assert.equal(left.sendCompletionControl(
      rightDescriptor.nodeRoutingId,
      [Buffer.concat([
        Buffer.from([0x5a, 0x4d, 1, M6aServiceWireCommand.livenessProbe, 0]),
        Buffer.alloc(64 * 1024)
      ])]
    ), false);

    const before = performance.now() + 10;
    const probe = left.tickLiveness(before);
    assert.equal(probe.probes.length, 1);
    const leftLiveness = left.liveness as unknown as {
      peers: Map<string, { outstandingProbe?: bigint }>;
    };
    await pollUntil(() => {
      right.progressCompletion();
      left.progressCompletion();
      return leftLiveness.peers.get(rightDescriptor.nodeRoutingId)
        ?.outstandingProbe === undefined;
    });

    const rightInternal = right as unknown as {
      expectedPeers: Map<string, {
        meshName: string;
        nodeRoutingId: string;
        endpoint: string;
        securityIdentity: string;
        lifecycleGeneration: bigint;
      }>;
    };
    rightInternal.expectedPeers.set(leftDescriptor.nodeRoutingId, {
      meshName: leftDescriptor.meshName,
      nodeRoutingId: leftDescriptor.nodeRoutingId,
      endpoint: leftDescriptor.advertisedEndpoint,
      securityIdentity: leftDescriptor.securityIdentity,
      lifecycleGeneration: leftDescriptor.lifecycleGeneration
    });
    assert.equal(left.sendCompletionControl(
      rightDescriptor.nodeRoutingId,
      [encodeRouteMeshAdmission(M6aServiceWireCommand.update, {
        ...leftDescriptor,
        lifecycleGeneration: leftDescriptor.lifecycleGeneration + 1n,
        descriptorRevision: leftDescriptor.descriptorRevision + 1n,
        state: 'serving'
      })]
    ), true);
    await new Promise(resolve => setTimeout(resolve, 20));
    right.progressCompletion();
    assert.equal(
      right.topology.peer(leftDescriptor.nodeRoutingId)
        ?.descriptor.lifecycleGeneration,
      leftDescriptor.lifecycleGeneration
    );

    // No Application Recv was started while Completion control progressed.
    assert.equal(right.mailbox.pendingMessages('application'), 0);
    await pollUntil(() => right.pumpOne() === 'application');
    assert.equal(right.mailbox.pendingMessages('application'), 1);
  } finally {
    left.close();
    right.close();
  }
});

test('one-sided endpoint-only client upgrades the provisional route before Ready', async () => {
  const endpointNonce = `${process.pid}-${Date.now()}`;
  const providerDescriptor = descriptor(
    'm6a-manual-provider',
    `ipc:///tmp/zlink-m6a-manual-provider-${endpointNonce}.sock`
  );
  const clientDescriptor = descriptor(
    'm6a-manual-client',
    `ipc:///tmp/zlink-m6a-manual-client-${endpointNonce}.sock`
  );
  const provider = new RawServiceMeshRuntime({ descriptor: providerDescriptor });
  const client = new RawServiceMeshRuntime({ descriptor: clientDescriptor });
  provider.start();
  client.start();
  try {
    client.connectPeerEndpoint(providerDescriptor.advertisedEndpoint);
    await pollUntil(() => {
      provider.drainMonitorEvents();
      client.drainMonitorEvents();
      provider.pumpOne();
      client.pumpOne();
      provider.progressCompletion();
      client.progressCompletion();
      provider.tickLiveness();
      client.tickLiveness();
      client.announceExpectedPeers();
      return client.isPeerRouteReady(providerDescriptor.nodeRoutingId)
        && provider.isPeerRouteReady(clientDescriptor.nodeRoutingId);
    });

    const pending = client.requestToNode(providerDescriptor.nodeRoutingId, {
      packetName: 'ManualEndpointQuestion',
      contentType: 'application/json',
      payload: Buffer.from('request')
    }, 2_000);
    await pollUntil(() => {
      provider.drainMonitorEvents();
      client.drainMonitorEvents();
      provider.pumpOne();
      client.pumpOne();
      provider.progressCompletion();
      client.progressCompletion();
      return provider.mailbox.pendingMessages('application') > 0;
    });
    const request = provider.mailbox.tryClaim('application', 1, 4_096)!;
    provider.reply(request.records[0]!, {
      packetName: 'ManualEndpointAnswer',
      contentType: 'application/json',
      payload: Buffer.from('reply')
    });
    assert.equal(provider.mailbox.release(request), true);
    const result = await awaitWithin(
      pending.promise,
      2_000,
      'Timed out waiting for one-sided manual endpoint reply.'
    );
    assert.equal(result.terminalResult, 0);
  } finally {
    client.close();
    provider.close();
  }
});

test('bilateral endpoint-only manual connections learn peer RIDs and converge', async () => {
  const endpointNonce = `${process.pid}-${Date.now()}`;
  const leftDescriptor = descriptor(
    'm6a-endpoint-left',
    `ipc:///tmp/zlink-m6a-endpoint-left-${endpointNonce}.sock`
  );
  const rightDescriptor = descriptor(
    'm6a-endpoint-right',
    `ipc:///tmp/zlink-m6a-endpoint-right-${endpointNonce}.sock`
  );
  const left = new RawServiceMeshRuntime({ descriptor: leftDescriptor });
  const right = new RawServiceMeshRuntime({ descriptor: rightDescriptor });
  right.start();
  try {
    right.connectPeerEndpoint(leftDescriptor.advertisedEndpoint);
    await new Promise(resolve => setTimeout(resolve, 20));
    left.start();
    left.connectPeerEndpoint(rightDescriptor.advertisedEndpoint);
    await pollUntil(() => {
      right.announceExpectedPeers();
      left.announceExpectedPeers();
      right.drainMonitorEvents();
      left.drainMonitorEvents();
      right.pumpOne();
      left.pumpOne();
      right.pumpOne();
      left.tickLiveness();
      right.tickLiveness();
      left.progressCompletion();
      right.progressCompletion();
      return left.topology.peer(rightDescriptor.nodeRoutingId) !== undefined
        && right.topology.peer(leftDescriptor.nodeRoutingId) !== undefined
        && left.isPeerRouteReady(rightDescriptor.nodeRoutingId)
        && right.isPeerRouteReady(leftDescriptor.nodeRoutingId)
        && left.topology.peer(rightDescriptor.nodeRoutingId)?.connectionDiscriminator
          === `initiator:${leftDescriptor.nodeRoutingId}`
        && right.topology.peer(leftDescriptor.nodeRoutingId)?.connectionDiscriminator
          === `initiator:${leftDescriptor.nodeRoutingId}`;
    });

    assert.equal(
      left.topology.peer(rightDescriptor.nodeRoutingId)?.descriptor.nodeRoutingId,
      rightDescriptor.nodeRoutingId
    );
    assert.equal(
      right.topology.peer(leftDescriptor.nodeRoutingId)?.descriptor.nodeRoutingId,
      leftDescriptor.nodeRoutingId
    );
    assert.equal(
      left.topology.peer(rightDescriptor.nodeRoutingId)?.connectionDiscriminator,
      `initiator:${leftDescriptor.nodeRoutingId}`
    );
    assert.equal(
      right.topology.peer(leftDescriptor.nodeRoutingId)?.connectionDiscriminator,
      `initiator:${leftDescriptor.nodeRoutingId}`
    );

    for (const [source, target, targetRid] of [
      [left, right, rightDescriptor.nodeRoutingId],
      [right, left, leftDescriptor.nodeRoutingId]
    ] as const) {
      const pending = source.requestToNode(targetRid, {
        packetName: 'BilateralQuestion',
        contentType: 'application/json',
        payload: Buffer.from('request')
      }, 2_000);
      await pollUntil(() => target.pumpOne() === 'application');
      const request = target.mailbox.tryClaim('application', 1, 4_096)!;
      target.reply(request.records[0]!, {
        packetName: 'BilateralAnswer',
        contentType: 'application/json',
        payload: Buffer.from('reply')
      });
      assert.equal(target.mailbox.release(request), true);
      const result = await awaitWithin(
        pending.promise,
        2_000,
        `Timed out waiting for '${targetRid}' bilateral reply.`
      );
      assert.equal(result.terminalResult, 0);
    }

    left.disconnectPeerEndpoint(rightDescriptor.advertisedEndpoint);
    assert.equal(left.topology.peer(rightDescriptor.nodeRoutingId), undefined);
  } finally {
    left.close();
    right.close();
  }
});

test('local channel requests preserve successful and failed terminal results', async () => {
  const local = new RawServiceMeshRuntime({
    descriptor: descriptor(
      'm6a-local-channel',
      `ipc:///tmp/zlink-m6a-local-channel-${process.pid}-${Date.now()}.sock`
    )
  });
  local.start();
  try {
    const success = local.requestToChannel('alpha', {
      packetName: 'Question',
      contentType: 'application/json',
      payload: Buffer.from('request')
    }, 2_000)!;
    const successClaim = local.mailbox.tryClaim('application', 1, 4096)!;
    local.reply(successClaim.records[0]!, {
      packetName: 'Answer',
      contentType: 'application/json',
      payload: Buffer.from('reply')
    });
    assert.equal(local.mailbox.release(successClaim), true);
    const successResult = await success.promise;
    assert.equal(successResult.terminalResult, 0);
    assert.equal(Buffer.from(successResult.payload!.payload).toString(), 'reply');

    const failure = local.requestToChannel('alpha', {
      packetName: 'MissingHandler',
      contentType: 'application/json',
      payload: Buffer.from('request')
    }, 2_000)!;
    const failureClaim = local.mailbox.tryClaim('application', 1, 4096)!;
    local.reply(
      failureClaim.records[0]!,
      {
        packetName: 'Ignored',
        contentType: 'application/json',
        payload: Buffer.from('must-not-be-returned')
      },
      102,
      7
    );
    assert.equal(local.mailbox.release(failureClaim), true);
    const failureResult = await failure.promise;
    assert.equal(failureResult.terminalResult, 102);
    assert.equal(failureResult.failureCode, 7);
    assert.equal(failureResult.payload, undefined);
  } finally {
    local.close();
  }
});

test('host-wide inbound byte budget pauses and resumes without dropping accepted work', () => {
  const budget = new ZLinkInboundDispatchBudget(8n);
  let resumed = 0;
  budget.onResume(() => resumed += 1);

  budget.enqueue(9n);
  assert.equal(budget.receivePaused, true);
  budget.start(9n);
  assert.deepEqual(budget.snapshot(), {
    applicationHwmBytes: 8n,
    pendingPayloadBytes: 9n,
    queuedPayloadBytes: 0n,
    activePayloadBytes: 9n,
    applicationReceivePaused: true,
    pendingCompletionSends: 0n,
    completionSendLimit: 65_536n
  });
  budget.complete(9n);

  assert.equal(resumed, 1);
  assert.equal(budget.receivePaused, false);
  assert.equal(budget.pendingPayloadBytes, 0n);
});

test('paused Application receive resumes from a terminal handler without polling', async () => {
  const budget = new ZLinkInboundDispatchBudget(8n);
  budget.enqueue(8n);
  budget.start(8n);

  let resumed = false;
  const waiting = budget.waitUntilResumed().then(() => {
    resumed = true;
  });
  await Promise.resolve();
  assert.equal(resumed, false);

  budget.complete(8n);
  await waiting;
  assert.equal(resumed, true);
});

test('completion send admission reports the real host-wide permit usage', async () => {
  const budget = new ZLinkInboundDispatchBudget(0n);
  const release = await budget.acquireCompletionSend();
  assert.equal(budget.snapshot().pendingCompletionSends, 1n);
  assert.equal(budget.snapshot().completionSendLimit, 65_536n);

  release();
  release();
  assert.equal(budget.snapshot().pendingCompletionSends, 0n);
});

test('completion send admission applies only to operations with a reply route', () => {
  assert.equal(operationRequiresReply(OperationKind.NodeRequest), true);
  assert.equal(operationRequiresReply(OperationKind.ChannelRequest), true);
  assert.equal(operationRequiresReply(OperationKind.SpotRequest), true);
  assert.equal(operationRequiresReply(OperationKind.ActorRequest), true);
  assert.equal(operationRequiresReply(OperationKind.InstanceSpotRequest), true);
  assert.equal(operationRequiresReply(OperationKind.ActorLookup), false);
  assert.equal(operationRequiresReply(OperationKind.ActorJoin), false);
  assert.equal(operationRequiresReply(OperationKind.UserSpotCreate), false);
});

test('Application HWM Auto uses the configured finite process memory limit', () => {
  assert.equal(resolveApplicationHwm({
    applicationHwmProfile: ZLinkApplicationHwmProfile.Balanced,
    processMemoryLimitBytes: 1_000n
  }), 100n);
});

test('Application HWM Auto uses the managed heap as an automatic candidate', () => {
  //  Spec 06: an unconfigured host still starts, and repeated resolution is stable.
  const resolved = resolveApplicationHwm({
    applicationHwmProfile: ZLinkApplicationHwmProfile.Balanced
  });
  assert.ok(resolved > 0n);
  assert.equal(resolved, resolveApplicationHwm({
    applicationHwmProfile: ZLinkApplicationHwmProfile.Balanced
  }));
  assert.ok(resolved <= BigInt(Math.floor(getHeapStatistics().heap_size_limit / 10)) + 1n);
  assert.ok(resolved <= BigInt(totalmem()));
});

async function pollUntil(condition: () => boolean): Promise<void> {
  const deadline = Date.now() + 2_000;
  while (Date.now() < deadline) {
    if (condition()) return;
    await new Promise(resolve => setTimeout(resolve, 1));
  }
  throw new Error('Timed out waiting for deterministic runtime progress.');
}

async function awaitWithin<T>(
  promise: Promise<T>,
  timeoutMs: number,
  message: string
): Promise<T> {
  let timeout: NodeJS.Timeout | undefined;
  try {
    return await Promise.race([
      promise,
      new Promise<never>((_, reject) => {
        timeout = setTimeout(() => reject(new Error(message)), timeoutMs);
      })
    ]);
  } finally {
    if (timeout !== undefined) clearTimeout(timeout);
  }
}
