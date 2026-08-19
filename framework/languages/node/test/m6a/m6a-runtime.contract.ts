import assert from 'node:assert/strict';
import { test } from 'node:test';
import { createContext } from '@zlink-systems/zlink';

import {
  SERVICE_WIRE_MAGIC,
  SERVICE_WIRE_MAJOR,
  SERVICE_WIRE_REQUIRED_CAPABILITY,
  ServiceWireCommand
} from '../../../../runtime/protocol/generated/node/service_wire_constants';
import {
  RawServiceMeshRuntime,
  type RawServiceMeshRuntimeOptions
} from '../../packages/framework/src/runtime/foundation/raw-service-mesh-runtime';
import {
  ZLinkNodeRawBindingPort
} from '../../packages/framework/src/runtime/backend/node/node-raw-binding-port';
import {
  ZLinkNodeRawMeshBackend
} from '../../packages/framework/src/runtime/backend/node/node-raw-mesh-backend';
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
  ApplicationJobQueue,
  resolveApplicationJobQueueConfiguration
} from '../../packages/framework/src/runtime/host/application-job-queue';
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
  encodeNodeRequestHeader,
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
    applicationVersion: 1n,
    protocolCapabilities: [SERVICE_WIRE_REQUIRED_CAPABILITY],
    objectRole: 'server',
    placementWeight: 100,
    activeCapacityLimit: 10_000,
    pendingCapacityLimit: 128,
    activeCapacityUsed: 0,
    pendingCapacityUsed: 0
  };
}

function rawServiceRuntime(
  options: Omit<RawServiceMeshRuntimeOptions, 'bindingPort'>
    & Partial<Pick<RawServiceMeshRuntimeOptions, 'bindingPort'>>
): RawServiceMeshRuntime {
  return new RawServiceMeshRuntime({
    ...options,
    bindingPort: options.bindingPort ?? new ZLinkNodeRawBindingPort(),
    applicationJobQueue: options.applicationJobQueue ?? applicationJobQueue()
  });
}

function applicationJobQueue(): ApplicationJobQueue {
  return new ApplicationJobQueue(
    resolveApplicationJobQueueConfiguration(
      { maxQueuedApplicationJobs: 2_048n },
      () => 1n
    )
  );
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
  offset += 8 + 8;
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

test('RouteMesh admission requires the generated capability but accepts sorted unknown extras', () => {
  const topology = new ServiceTopologyRegistry(descriptor('local'));
  assert.equal(topology.admit({
    ...descriptor('peer'),
    protocolCapabilities: [SERVICE_WIRE_REQUIRED_CAPABILITY, 'object-type:quest']
  }, 'connection-extra'), 'admitted');
  assert.throws(() => new ServiceTopologyRegistry({
    ...descriptor('old'),
    protocolCapabilities: ['framework-service-v12']
  }), /framework-service-v13/);
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

test('RouteMesh admission preserves an optional maintenance wave across updates', () => {
  const initial = {
    ...descriptor('wave-peer'),
    maintenanceWave: 'rolling-a'
  };
  const frame = encodeRouteMeshAdmission(M6aServiceWireCommand.update, initial);
  assert.equal(
    decodeRouteMeshAdmission(frame, M6aServiceWireCommand.update, initial.nodeRoutingId)
      .maintenanceWave,
    'rolling-a'
  );

  const topology = new ServiceTopologyRegistry(descriptor('wave-local'));
  assert.equal(topology.admit(initial, 'connection-a'), 'admitted');
  assert.equal(topology.admit({
    ...initial,
    descriptorRevision: 2n,
    maintenanceWave: 'rolling-b'
  }, 'connection-a'), 'admitted');
  assert.equal(topology.peer(initial.nodeRoutingId)?.descriptor.maintenanceWave, 'rolling-b');

  const backend = new ZLinkNodeRawMeshBackend(
    'wave-mesh',
    'wave-node',
    new ZLinkNodeRawBindingPort(),
    applicationJobQueue()
  );
  backend.configureObjectPlacement({
    role: 'server',
    placementWeight: 100,
    activeCapacityLimit: 10,
    pendingCapacityLimit: 2,
    objectCapabilities: ['object-type:player'],
    maintenanceWave: 'rolling-a'
  });
  const created = (backend as unknown as { createDescriptor(): ServiceNodeDescriptor })
    .createDescriptor();
  assert.equal(created.maintenanceWave, 'rolling-a');
});

test('raw binding receive retains Core credit until the Framework record closes', async () => {
  const context = createContext();
  const binding = new ZLinkNodeRawBindingPort(context);
  const host = binding.createHost();
  const router = host.createRouter();
  const dealer = host.createDealer();
  const endpoint = `ipc:///tmp/zlink-node-raw-retained-${process.pid}-${Date.now()}.sock`;
  router.setRoutingId('raw-retained-router');
  router.bind(endpoint);
  dealer.setRoutingId('raw-retained-dealer');
  dealer.connect(endpoint);
  try {
    await pollUntil(async () => {
      try {
        await dealer.send([Buffer.from('retained-payload')]);
        return true;
      } catch {
        return false;
      }
    });
    let received;
    await pollUntil(() => {
      received = router.receive(true);
      return received !== undefined;
    });
    assert.equal(
      context.getCoreHwmBudgetSnapshot().outstandingApplicationLeaseCount,
      1n
    );
    received!.close();
    assert.equal(
      context.getCoreHwmBudgetSnapshot().outstandingApplicationLeaseCount,
      0n
    );
  } finally {
    host.close();
    context.close();
  }
});

test('RouteMesh admission rejects malformed UTF-8 instead of replacing bytes', () => {
  const meshName = 'canonical-utf8-mesh';
  const frame = Buffer.from(encodeRouteMeshAdmission(
    M6aServiceWireCommand.update,
    { ...descriptor('utf8-peer'), meshName }
  ));
  const meshOffset = frame.indexOf(Buffer.from(meshName, 'utf8'));
  assert.ok(meshOffset > 0);
  frame[meshOffset] = 0xff;
  assert.throws(
    () => decodeRouteMeshAdmission(frame, M6aServiceWireCommand.update, 'utf8-peer'),
    error => error instanceof Error
      && error.name === 'ServiceWireProtocolError'
      && /meshName/.test(error.message)
  );
});

//  GOLDEN — service-wire-v1.schema.json reply(20) byte layout.
//  `request-specific-tail` is a conditional-union WITHOUT `bodyLengthType`, so
//  the tail is inline: prefix(5) + u64 correlation + u32 terminalResult +
//  u32 failureCode + tail. Empty tail == exactly 21 bytes, no u16 length.
//  These vectors are byte-identical across C++/Java/Node/.NET.
test('golden: reply header pins the inline schema tail byte layout', () => {
  assert.equal(
    encodeReplyHeader(7n).toString('hex'),
    '5a4d01140000000000000000070000000000000000'
  );
  assert.equal(encodeReplyHeader(7n).byteLength, 21);
  assert.equal(
    encodeReplyHeader(8n, 102, 14, Uint8Array.from([1, 2, 3])).toString('hex'),
    '5a4d0114000000000000000008000000660000000e010203'
  );
  assert.equal(
    encodeReplyHeader(8n, 102, 14, Uint8Array.from([1, 2, 3])).byteLength,
    24
  );
});

test('reply header round-trips the inline schema tail', () => {
  const empty = encodeReplyHeader(7n);
  assert.equal(empty.byteLength, 21);
  assert.deepEqual(decodeReplyHeader(empty), {
    correlation: 7n,
    terminalResult: 0,
    failureCode: 0,
    tail: Buffer.alloc(0)
  });

  //  102+14 (requestTargetNotFound -> notFound) is a schema-legal exact pair;
  //  the previous 102+17 row predated the terminal-failure-integrity check
  //  (17 requestFailed pairs with 105 internalError).
  const tail = encodeReplyHeader(8n, 102, 14, Uint8Array.from([1, 2, 3]));
  assert.deepEqual(decodeReplyHeader(tail), {
    correlation: 8n,
    terminalResult: 102,
    failureCode: 14,
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

test('RouteMesh admission classifies stale and conflicting descriptor revisions as protocol errors', async () => {
  const runtime = rawServiceRuntime({ descriptor: descriptor('local') });
  const current = {
    ...descriptor('peer'),
    descriptorRevision: 2n,
    state: 'serving' as const
  };
  assert.equal(runtime.topology.admit(current, 'connection-a'), 'admitted');
  const processReceived = (runtime as unknown as {
    processReceived(record: {
      sourceRid: string;
      parts: readonly Buffer[];
    }, nowMs: number): Promise<string>;
  }).processReceived.bind(runtime);

  assert.equal(await processReceived({
    sourceRid: current.nodeRoutingId,
    parts: [encodeRouteMeshAdmission(M6aServiceWireCommand.update, {
      ...current,
      descriptorRevision: 1n
    })]
  }, performance.now()), 'protocolError');
  assert.equal(await processReceived({
    sourceRid: current.nodeRoutingId,
    parts: [encodeRouteMeshAdmission(M6aServiceWireCommand.update, {
      ...current,
      channels: [{ name: 'alpha', weight: 99 }]
    })]
  }, performance.now()), 'protocolError');
  assert.equal(runtime.topology.peer(current.nodeRoutingId)?.descriptor.descriptorRevision, 2n);
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
    protocolCapabilities: [SERVICE_WIRE_REQUIRED_CAPABILITY, 'object-type:quest']
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
    protocolCapabilities: [SERVICE_WIRE_REQUIRED_CAPABILITY, 'object-type:quest'],
    activeCapacityLimit: 1,
    activeCapacityUsed: 1
  });
  assert.equal(exhausted.objectPlacementStatus('quest'), 'capacity');

  const filtered = new ServiceTopologyRegistry({
    ...descriptor('zero-weight-available'),
    state: 'serving',
    placementWeight: 0,
    protocolCapabilities: [SERVICE_WIRE_REQUIRED_CAPABILITY, 'object-type:quest']
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
      applicationVersion: peer.applicationVersion + 1n
    },
    {
      ...peer,
      descriptorRevision: 2n,
      protocolCapabilities: [SERVICE_WIRE_REQUIRED_CAPABILITY, 'object-type:other']
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

test('raw monitor preserves each physical candidate direction through admission and disconnect fencing', async () => {
  const runtime = rawServiceRuntime({ descriptor: descriptor('local') });
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

  assert.equal(await runtime.drainMonitorEvents(), 2);
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
  assert.equal(await runtime.drainMonitorEvents(), 1);
  assert.equal(runtime.topology.peer('peer')?.connectionId, inbound.connectionId);
  assert.equal(internal.connectionCandidates.get('peer')?.size, 1);
  assert.equal(internal.connectionIds.get('peer'), inbound.connectionId);
});

test('raw monitor ignores a late disconnect from the superseded physical connection', async () => {
  const runtime = rawServiceRuntime({ descriptor: descriptor('local') });
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

  assert.equal(await runtime.drainMonitorEvents(), 1);
  assert.equal(runtime.topology.peer('peer')?.connectionId, currentConnection);
  assert.equal(internal.connectionIds.get('peer'), currentConnection);
});

test('raw monitor fences paired transport lanes and ignores ready-count snapshots', async () => {
  const runtime = rawServiceRuntime({ descriptor: descriptor('local') });
  const peer = { ...descriptor('peer-paired'), state: 'serving' as const };
  const internal = runtime as unknown as {
    connectionCandidates: Map<string, Map<string, { connectionId: string }>>;
    monitorEvents: Array<{
      event: number;
      value: number;
      routingId: string;
      localAddress: string;
      remoteAddress: string;
      connectionId?: bigint;
      transportPairId?: bigint;
      transportPairGeneration?: bigint;
      transportLane?: number;
      flags?: number;
    }>;
  };
  const pairId = 41n;
  const pairGeneration = 3n;
  internal.monitorEvents.push(
    {
      event: 0x1000,
      value: 1,
      routingId: peer.nodeRoutingId,
      localAddress: 'tcp://local:41001',
      remoteAddress: peer.advertisedEndpoint,
      connectionId: 101n,
      transportPairId: pairId,
      transportPairGeneration: pairGeneration,
      transportLane: 1,
      flags: 1
    },
    {
      event: 0x1000,
      value: 2,
      routingId: peer.nodeRoutingId,
      localAddress: 'tcp://local:41001',
      remoteAddress: peer.advertisedEndpoint,
      connectionId: 102n,
      transportPairId: pairId,
      transportPairGeneration: pairGeneration,
      transportLane: 0,
      flags: 0
    }
  );

  assert.equal(await runtime.drainMonitorEvents(), 2);
  assert.equal(internal.connectionCandidates.get(peer.nodeRoutingId)?.size, 1);
  const connectionId = [...internal.connectionCandidates.get(peer.nodeRoutingId)!.keys()][0]!;
  assert.equal(
    runtime.topology.admit(peer, connectionId),
    'admitted'
  );

  internal.monitorEvents.push({
    event: 0x0200,
    value: 3,
    routingId: peer.nodeRoutingId,
    localAddress: 'tcp://completion-local:51001',
    remoteAddress: 'tcp://completion-remote:52001',
    connectionId: 104n,
    transportPairId: pairId,
    transportPairGeneration: pairGeneration,
    transportLane: 1,
    flags: 0
  });
  assert.equal(await runtime.drainMonitorEvents(), 1);
  assert.equal(runtime.topology.peer(peer.nodeRoutingId), undefined);

  internal.monitorEvents.push({
    event: 0x0200,
    value: 4,
    routingId: peer.nodeRoutingId,
    localAddress: 'tcp://different-local:51001',
    remoteAddress: 'tcp://different-remote:52001',
    connectionId: 103n,
    transportPairId: pairId,
    transportPairGeneration: pairGeneration,
    transportLane: 0,
    flags: 0
  });
  assert.equal(await runtime.drainMonitorEvents(), 1);
  assert.equal(runtime.topology.peer(peer.nodeRoutingId), undefined);
});

test('raw disconnect fences a late lifecycle generation after peer replacement', () => {
  const endpoint = `ipc:///tmp/zlink-m6a-generation-fence-${process.pid}-${Date.now()}.sock`;
  const runtime = rawServiceRuntime({
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
  const runtime = rawServiceRuntime({
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

test('runtime weight changes increment the local descriptor revision and preserve public bounds', async () => {
  const runtime = rawServiceRuntime({
    descriptor: {
      ...descriptor('runtime-options'),
      state: 'serving'
    }
  });
  const initial = runtime.topology.localDescriptor();
  await runtime.updateLocalWeights({ placementWeight: 0 });
  const placement = runtime.topology.localDescriptor();
  assert.equal(placement.descriptorRevision, initial.descriptorRevision + 1n);
  assert.equal(placement.placementWeight, 0);

  await runtime.updateLocalWeights({
    channelName: 'alpha',
    channelWeight: 10_000
  });
  const channel = runtime.topology.localDescriptor();
  assert.equal(channel.descriptorRevision, placement.descriptorRevision + 1n);
  assert.equal(channel.channels.find(candidate => candidate.name === 'alpha')?.weight, 10_000);
});

test('mailbox has no hidden admission cap and owner claims progress independently', () => {
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
  }), true);
  assert.equal(mailbox.tryEnqueue({
    owner: 'peer-a',
    domain: 'infrastructure',
    parts: [Buffer.from([9])]
  }), true);

  const application = mailbox.tryClaim('application', 1, 8)!;
  const parallelOwner = mailbox.tryClaim('application', 1, 8)!;
  assert.equal(parallelOwner.owner, 'spot-b');
  mailbox.releaseClaimedPayload(application.records[0]!);
  assert.equal(application.records[0]!.parts.length, 0);
  const infrastructure = mailbox.tryClaim('infrastructure', 1, 8)!;
  assert.equal(infrastructure.records.length, 1);
  assert.equal(mailbox.release(infrastructure), true);
  assert.equal(mailbox.release(infrastructure), false);
  assert.equal(mailbox.release(parallelOwner), true);
  assert.equal(mailbox.release(application), true);
  assert.equal(mailbox.tryClaim('application', 1, 8)?.records.length, 1);
});

test('remote request is not rejected by legacy mailbox limits after shared admission', async () => {
  const jobs = applicationJobQueue();
  const runtime = rawServiceRuntime({
    descriptor: descriptor('local'),
    mailbox: { applicationMessages: 1, applicationBytes: 4_096 },
    applicationJobQueue: jobs
  });
  assert.equal(runtime.topology.admit({
    ...descriptor('peer'),
    state: 'serving'
  }, 'peer-connection'), 'admitted');
  assert.equal(runtime.mailbox.tryEnqueue({
    owner: 'node:occupied',
    domain: 'application',
    parts: [Buffer.from('occupied')]
  }), true);
  let reply: readonly Uint8Array[] | undefined;
  const internals = runtime as unknown as {
    processReceived(
      received: {
        sourceRid: string;
        requestSeq: bigint;
        parts: readonly Buffer[];
        reply(parts: readonly Uint8Array[]): void;
      },
      nowMs: number,
      applicationJobOwner: Awaited<ReturnType<RawServiceMeshRuntime['reserveLocalIngress']>>
    ): Promise<string>;
  };
  const applicationJobOwner = await runtime.reserveLocalIngress();
  const result = await internals.processReceived({
    sourceRid: 'peer',
    requestSeq: 19n,
    parts: [
      encodeNodeRequestHeader(7n),
      encodeApplicationPayload({
        packetName: 'Blocked',
        contentType: 'application/octet-stream',
        payload: Buffer.from('payload')
      })
    ],
    reply(parts) { reply = parts; }
  }, 0, applicationJobOwner);
  applicationJobOwner.close();

  assert.equal(result, 'application');
  assert.equal(reply, undefined);
  assert.equal(runtime.mailbox.pendingMessages('application'), 2);
  runtime.close();
});

test('raw protocol errors reply when correlation is recoverable and always report diagnostics', () => {
  const observed: Array<{
    sourceRoutingId: string;
    request: boolean;
    replied: boolean;
    command?: number;
  }> = [];
  const runtime = rawServiceRuntime({
    descriptor: descriptor('local'),
    onProtocolError: record => observed.push(record)
  });
  runtime.start();
  let reply: readonly Uint8Array[] | undefined;
  const internals = runtime as unknown as {
    reportProtocolError(received: {
      sourceRid: string;
      requestSeq?: bigint;
      parts: readonly Buffer[];
      reply?(parts: readonly Uint8Array[]): void;
    }): void;
  };

  internals.reportProtocolError({
    sourceRid: 'peer-request',
    requestSeq: 41n,
    parts: [encodeNodeRequestHeader(9n)],
    reply(parts) { reply = parts; }
  });
  assert.ok(reply);
  assert.deepEqual(decodeReplyHeader(reply[0]!), {
    correlation: 9n,
    terminalResult: 104,
    failureCode: 16,
    tail: Buffer.alloc(0)
  });

  internals.reportProtocolError({
    sourceRid: 'peer-send',
    parts: [Buffer.from([0xff])]
  });
  assert.deepEqual(observed, [
    {
      sourceRoutingId: 'peer-request',
      request: true,
      replied: true,
      command: M6aServiceWireCommand.nodeRequest
    },
    { sourceRoutingId: 'peer-send', request: false, replied: false }
  ]);
  runtime.close();
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
  const left = rawServiceRuntime({ descriptor: leftDescriptor });
  const right = rawServiceRuntime({ descriptor: rightDescriptor });
  left.start();
  right.start();
  try {
    left.connectPeer(rightDescriptor.advertisedEndpoint, rightDescriptor);
    await pollUntil(async () => {
      await left.announceExpectedPeers();
      await right.pumpOne();
      await left.pumpOne();
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
    'client-monitor',
    new ZLinkNodeRawBindingPort(),
    applicationJobQueue()
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
  const left = rawServiceRuntime({ descriptor: leftDescriptor });
  const right = rawServiceRuntime({ descriptor: rightDescriptor });
  left.start();
  right.start();
  try {
    left.connectPeer(rightDescriptor.advertisedEndpoint, rightDescriptor);
    await pollUntil(async () => {
      await left.announceExpectedPeers();
      await right.pumpOne();
      await left.pumpOne();
      await left.tickLiveness();
      await right.tickLiveness();
      return left.topology.peer('m6a-right') !== undefined
        && right.topology.peer('m6a-left') !== undefined
        && left.isPeerRouteReady('m6a-right')
        && right.isPeerRouteReady('m6a-left');
    });
    assert.equal(left.isPeerRouteReady('m6a-right', rightDescriptor.lifecycleGeneration), true);
    assert.equal(left.isPeerRouteReady('m6a-right', rightDescriptor.lifecycleGeneration + 1n), false);

    assert.equal(await left.sendToChannel('alpha', {
      packetName: 'ChannelNotice',
      contentType: 'application/json',
      payload: Buffer.from('notice')
    }), true);
    let observedSourceRoutingId: string | undefined;
    let observedByteCount = 0;
    await pollUntil(async () => await right.pumpOne(
      performance.now(),
      (sourceRoutingId, byteCount) => {
        observedSourceRoutingId = sourceRoutingId;
        observedByteCount = byteCount;
      }
    ) === 'application');
    assert.equal(observedSourceRoutingId, 'm6a-left');
    assert.ok(observedByteCount > Buffer.byteLength('notice'));
    const sent = right.mailbox.tryClaim('application', 1, 4096)!;
    assert.equal(sent.owner, 'channel:alpha');
    assert.equal(right.mailbox.release(sent), true);

    const pending = left.requestToNode('m6a-right', {
      packetName: 'Question',
      contentType: 'application/json',
      payload: Buffer.from('request')
    }, 2_000);
    await pollUntil(async () => await right.pumpOne() === 'application');
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

  const backend = new ZLinkNodeRawMeshBackend(
    'm6a-mesh',
    'backend-host',
    new ZLinkNodeRawBindingPort(),
    applicationJobQueue()
  );
  backend.setBind(`ipc:///tmp/zlink-m6a-backend-${process.pid}-${Date.now()}.sock`);
  backend.addChannelName('alpha');
  backend.start();
  try {
    assert.equal(backend.status().state, 2);
    const initialRevision = backend.status().descriptorRevision;
    await backend.setPlacementWeight(0);
    assert.equal(backend.status().descriptorRevision, initialRevision + 1n);
    await backend.setChannelWeight('alpha', 0);
    assert.equal(backend.status().descriptorRevision, initialRevision + 2n);
    await assert.rejects(backend.setPlacementWeight(-1), /0\.\.10000/);
    await assert.rejects(
      backend.setChannelWeight('alpha', 10_001),
      /0\.\.10000/
    );
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

//  Spec 33-core-hwm-application-job-flow §8 (R6): a terminal reply completion
//  progresses independently of ordinary job-flow saturation. The requester's
//  ApplicationJobQueue is saturated at limit 1 (permit held, the ordinary
//  receive pump parked pre-receive as a capacity waiter), yet the wire reply
//  for an outstanding request still completes — reply completions resolve on
//  the native request path and never pass the application admission gate.
test('terminal reply completion progresses while ordinary job flow is saturated', async () => {
  const endpointNonce = `${process.pid}-${Date.now()}`;
  const leftDescriptor = descriptor(
    'm6a-r6-left',
    `ipc:///tmp/zlink-m6a-r6-left-${endpointNonce}.sock`
  );
  const rightDescriptor = descriptor(
    'm6a-r6-right',
    `ipc:///tmp/zlink-m6a-r6-right-${endpointNonce}.sock`
  );
  const leftJobs = new ApplicationJobQueue(resolveApplicationJobQueueConfiguration(
    { maxQueuedApplicationJobs: 1n },
    () => 1n
  ));
  const left = rawServiceRuntime({
    descriptor: leftDescriptor,
    applicationJobQueue: leftJobs
  });
  const right = rawServiceRuntime({ descriptor: rightDescriptor });
  left.start();
  right.start();
  try {
    left.connectPeer(rightDescriptor.advertisedEndpoint, rightDescriptor);
    await pollUntil(async () => {
      await left.announceExpectedPeers();
      await right.pumpOne();
      await left.pumpOne();
      await left.tickLiveness();
      await right.tickLiveness();
      return left.topology.peer('m6a-r6-right') !== undefined
        && right.topology.peer('m6a-r6-left') !== undefined
        && left.isPeerRouteReady('m6a-r6-right')
        && right.isPeerRouteReady('m6a-r6-left');
    });
    //  Drain any admission-handshake leftovers so the saturated pump below
    //  parks in front of exactly one ordinary application record.
    while (await left.pumpOne() !== 'noData') { /* drain */ }

    //  Saturate the requester's ordinary job flow: hold its only permit and
    //  park the receive pump as a capacity waiter.
    const occupied = await leftJobs.acquire();
    const parked = left.pumpOne();
    await new Promise(resolve => setImmediate(resolve));
    assert.equal(leftJobs.snapshot().capacityWaiters, 1n);
    assert.equal(leftJobs.snapshot().permitsInUse, 1n);

    //  An ordinary inbound record cannot be received while saturated.
    assert.equal(await right.sendToNode('m6a-r6-left', {
      packetName: 'OrdinaryNotice',
      contentType: 'application/json',
      payload: Buffer.from('parked')
    }), true);

    //  The terminal reply completion still progresses.
    const pending = left.requestToNode('m6a-r6-right', {
      packetName: 'Question',
      contentType: 'application/json',
      payload: Buffer.from('request')
    }, 2_000);
    await pollUntil(async () => await right.pumpOne() === 'application');
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
      'Timed out waiting for the reply terminal under saturation.'
    );
    assert.equal(result.terminalResult, 0);
    assert.equal(Buffer.from(result.payload!.payload).toString(), 'reply');
    //  The ordinary job flow stayed saturated the whole time: the reply
    //  terminal did not consume (or wait for) an application permit.
    assert.equal(leftJobs.snapshot().capacityWaiters, 1n);
    assert.equal(leftJobs.snapshot().permitsInUse, 1n);

    //  Releasing the permit drains the parked ordinary record normally.
    occupied.releaseAfterInternalProcessing();
    const parkedResult = await parked;
    assert.ok(parkedResult === 'application' || parkedResult === 'noData');
    if (parkedResult !== 'application') {
      await pollUntil(async () => await left.pumpOne() === 'application');
    }
    const drained = left.mailbox.tryClaim('application', 1, Number.MAX_SAFE_INTEGER)!;
    assert.equal(drained.owner, 'node:m6a-r6-left');
    const record = drained.records[0]!;
    record.applicationJob!.releaseBeforeHandler();
    record.applicationJob!.close();
    assert.equal(left.mailbox.release(drained), true);
    assert.equal(leftJobs.snapshot().permitsInUse, 0n);
  } finally {
    left.close();
    right.close();
  }
});

test('normal receive pump carries application and liveness traffic on one route', async () => {
  const endpointNonce = `${process.pid}-${Date.now()}`;
  const leftDescriptor = descriptor(
    'completion-left',
    `ipc:///tmp/zlink-m6a-completion-left-${endpointNonce}.sock`
  );
  const rightDescriptor = descriptor(
    'completion-right',
    `ipc:///tmp/zlink-m6a-completion-right-${endpointNonce}.sock`
  );
  const left = rawServiceRuntime({
    descriptor: leftDescriptor,
    probeIntervalMs: 1,
    peerTimeoutMs: 5_000
  });
  const right = rawServiceRuntime({
    descriptor: rightDescriptor,
    probeIntervalMs: 1,
    peerTimeoutMs: 5_000
  });
  left.start();
  right.start();
  try {
    left.connectPeer(rightDescriptor.advertisedEndpoint, rightDescriptor);
    await pollUntil(async () => {
      await left.announceExpectedPeers();
      await right.pumpOne();
      await left.pumpOne();
      return left.topology.peer(rightDescriptor.nodeRoutingId) !== undefined
        && right.topology.peer(leftDescriptor.nodeRoutingId) !== undefined;
    });

    assert.equal(await left.sendToNode(rightDescriptor.nodeRoutingId, {
      packetName: 'ApplicationOnNormalRoute',
      contentType: 'application/octet-stream',
      payload: Buffer.from('normal-route')
    }), true);

    const before = performance.now() + 10;
    const probe = await left.tickLiveness(before);
    assert.equal(probe.probes.length, 1);
    const leftLiveness = left.liveness as unknown as {
      peers: Map<string, { outstandingProbe?: bigint }>;
    };
    await pollUntil(async () => {
      await right.pumpOne();
      await left.pumpOne();
      return leftLiveness.peers.get(rightDescriptor.nodeRoutingId)
        ?.outstandingProbe === undefined;
    });
    assert.equal(right.mailbox.pendingMessages('application'), 1);

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
    assert.equal(await left.sendService(
      rightDescriptor.nodeRoutingId,
      [encodeRouteMeshAdmission(M6aServiceWireCommand.update, {
        ...leftDescriptor,
        lifecycleGeneration: leftDescriptor.lifecycleGeneration + 1n,
        descriptorRevision: leftDescriptor.descriptorRevision + 1n,
        state: 'serving'
      })]
    ), true);
    await pollUntil(async () => await right.pumpOne() === 'infrastructure');
    assert.equal(
      right.topology.peer(leftDescriptor.nodeRoutingId)
        ?.descriptor.lifecycleGeneration,
      leftDescriptor.lifecycleGeneration
    );
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
  const provider = rawServiceRuntime({ descriptor: providerDescriptor });
  const client = rawServiceRuntime({ descriptor: clientDescriptor });
  provider.start();
  client.start();
  try {
    client.connectPeerEndpoint(providerDescriptor.advertisedEndpoint);
    await pollUntil(async () => {
      await provider.drainMonitorEvents();
      await client.drainMonitorEvents();
      await provider.pumpOne();
      await client.pumpOne();
      await provider.tickLiveness();
      await client.tickLiveness();
      await client.announceExpectedPeers();
      return client.isPeerRouteReady(providerDescriptor.nodeRoutingId)
        && provider.isPeerRouteReady(clientDescriptor.nodeRoutingId);
    });

    const pending = client.requestToNode(providerDescriptor.nodeRoutingId, {
      packetName: 'ManualEndpointQuestion',
      contentType: 'application/json',
      payload: Buffer.from('request')
    }, 2_000);
    await pollUntil(async () => {
      await provider.drainMonitorEvents();
      await client.drainMonitorEvents();
      await provider.pumpOne();
      await client.pumpOne();
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
  const left = rawServiceRuntime({ descriptor: leftDescriptor });
  const right = rawServiceRuntime({ descriptor: rightDescriptor });
  right.start();
  try {
    right.connectPeerEndpoint(leftDescriptor.advertisedEndpoint);
    await new Promise(resolve => setTimeout(resolve, 20));
    left.start();
    left.connectPeerEndpoint(rightDescriptor.advertisedEndpoint);
    await pollUntil(async () => {
      await right.announceExpectedPeers();
      await left.announceExpectedPeers();
      await right.drainMonitorEvents();
      await left.drainMonitorEvents();
      await right.pumpOne();
      await left.pumpOne();
      await right.pumpOne();
      await left.tickLiveness();
      await right.tickLiveness();
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
      await pollUntil(async () => await target.pumpOne() === 'application');
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
  const local = rawServiceRuntime({
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
    let successClaim!: NonNullable<ReturnType<ServiceMailbox['tryClaim']>>;
    await pollUntil(() => {
      const claimed = local.mailbox.tryClaim('application', 1, 4096);
      if (claimed === undefined) return false;
      successClaim = claimed;
      return true;
    });
    local.reply(successClaim.records[0]!, {
      packetName: 'Answer',
      contentType: 'application/json',
      payload: Buffer.from('reply')
    });
    successClaim.records[0]!.applicationJob?.close();
    assert.equal(local.mailbox.release(successClaim), true);
    const successResult = await success.promise;
    assert.equal(successResult.terminalResult, 0);
    assert.equal(Buffer.from(successResult.payload!.payload).toString(), 'reply');

    const failure = local.requestToChannel('alpha', {
      packetName: 'MissingHandler',
      contentType: 'application/json',
      payload: Buffer.from('request')
    }, 2_000)!;
    let failureClaim!: NonNullable<ReturnType<ServiceMailbox['tryClaim']>>;
    await pollUntil(() => {
      const claimed = local.mailbox.tryClaim('application', 1, 4096);
      if (claimed === undefined) return false;
      failureClaim = claimed;
      return true;
    });
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
    failureClaim.records[0]!.applicationJob?.close();
    assert.equal(local.mailbox.release(failureClaim), true);
    const failureResult = await failure.promise;
    assert.equal(failureResult.terminalResult, 102);
    assert.equal(failureResult.failureCode, 7);
    assert.equal(failureResult.payload, undefined);
  } finally {
    local.close();
  }
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

async function pollUntil(
  condition: () => boolean | Promise<boolean>
): Promise<void> {
  const deadline = Date.now() + 2_000;
  while (Date.now() < deadline) {
    if (await condition()) return;
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
