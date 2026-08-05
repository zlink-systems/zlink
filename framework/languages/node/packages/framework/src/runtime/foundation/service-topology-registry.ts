import { descriptorConnectionNotRequired } from './route-mesh-connection-policy';
import { SmoothWeightedSelection } from './service-weighted-selection';

export type ServiceNodeState =
  | 'preparing'
  | 'serving'
  | 'retiring'
  | 'draining'
  | 'stopped'
  | 'error';

export type ServiceObjectRole = 'none' | 'client' | 'server';

export interface ServiceChannelDescriptor {
  readonly name: string;
  readonly weight: number;
}

export interface ServiceNodeDescriptor {
  readonly meshName: string;
  readonly nodeRoutingId: string;
  readonly lifecycleGeneration: bigint;
  readonly descriptorRevision: bigint;
  readonly advertisedEndpoint: string;
  readonly channels: readonly ServiceChannelDescriptor[];
  readonly state: ServiceNodeState;
  readonly securityIdentity: string;
  readonly effectiveMaxMessageBytes: number;
  readonly applicationVersion: bigint;
  readonly protocolCapabilities: readonly string[];
  readonly objectRole: ServiceObjectRole;
  readonly placementWeight: number;
  readonly activeCapacityLimit: number;
  readonly pendingCapacityLimit: number;
  readonly activeCapacityUsed: number;
  readonly pendingCapacityUsed: number;
}

export interface AdmittedServicePeer {
  readonly descriptor: ServiceNodeDescriptor;
  readonly connectionId: string;
  readonly connectionDiscriminator: string;
}

export interface ServicePeerAdmissionExpectation {
  readonly endpoint?: string;
  readonly securityIdentity?: string;
  readonly lifecycleGeneration?: bigint;
}

export type PeerAdmissionResult =
  | 'admitted'
  | 'notRequired'
  | 'meshMismatch'
  | 'invalidDescriptor'
  | 'staleDescriptor';

export type ServiceObjectPlacementStatus =
  | 'available'
  | 'unsupported'
  | 'capacity'
  | 'unavailable';

const REQUIRED_CAPABILITY = 'framework-service-v11';
const MAX_CAPACITY = 0x7fff_ffff;
const MAX_PUBLIC_WEIGHT = 10_000;

interface ServiceSelectionCacheEntry {
  readonly selection: SmoothWeightedSelection<unknown>;
}

/** Owns immutable peer snapshots and fences late physical-connection events. */
export class ServiceTopologyRegistry {
  private local: ServiceNodeDescriptor;
  private readonly peersByRid = new Map<string, AdmittedServicePeer>();
  private readonly notRequiredByRid = new Map<string, ServiceNodeDescriptor>();
  private readonly knownByRid = new Map<string, ServiceNodeDescriptor>();
  private readonly selections = new Map<string, ServiceSelectionCacheEntry>();

  constructor(local: ServiceNodeDescriptor) {
    validateDescriptor(local);
    this.local = cloneDescriptor(local);
  }

  localDescriptor(): ServiceNodeDescriptor {
    return cloneDescriptor(this.local);
  }

  publishLocal(descriptor: ServiceNodeDescriptor): void {
    validateDescriptor(descriptor);
    if (
      descriptor.meshName !== this.local.meshName
      || descriptor.nodeRoutingId !== this.local.nodeRoutingId
      || descriptor.lifecycleGeneration !== this.local.lifecycleGeneration
    ) {
      throw new TypeError('Published descriptor changes the local node identity.');
    }
    if (descriptor.descriptorRevision <= this.local.descriptorRevision) {
      throw new RangeError('Published descriptor revision must increase.');
    }
    this.local = cloneDescriptor(descriptor);
    for (const [nodeRoutingId, remote] of this.notRequiredByRid) {
      if (!descriptorConnectionNotRequired(this.local, remote)) {
        this.notRequiredByRid.delete(nodeRoutingId);
      }
    }
    this.rebuildSelections();
  }

  admit(
    descriptor: ServiceNodeDescriptor,
    connectionId: string,
    expected?: ServicePeerAdmissionExpectation,
    connectionDiscriminator = connectionId
  ): PeerAdmissionResult {
    try {
      validateDescriptor(descriptor);
      requireText(connectionId, 'connectionId');
      requireText(connectionDiscriminator, 'connectionDiscriminator');
    } catch (error) {
      return 'invalidDescriptor';
    }
    if (descriptor.meshName !== this.local.meshName) return 'meshMismatch';
    if (descriptor.nodeRoutingId === this.local.nodeRoutingId) return 'invalidDescriptor';
    if (
      (expected?.endpoint !== undefined
        && descriptor.advertisedEndpoint !== expected.endpoint)
      || (expected?.securityIdentity !== undefined
        && descriptor.securityIdentity !== expected.securityIdentity)
      || (expected?.lifecycleGeneration !== undefined
        && descriptor.lifecycleGeneration !== expected.lifecycleGeneration)
    ) {
      return 'invalidDescriptor';
    }
    if (descriptorConnectionNotRequired(this.local, descriptor)) {
      this.peersByRid.delete(descriptor.nodeRoutingId);
      this.remember(descriptor);
      this.notRequiredByRid.set(descriptor.nodeRoutingId, cloneDescriptor(descriptor));
      this.rebuildSelections();
      return 'notRequired';
    }
    this.notRequiredByRid.delete(descriptor.nodeRoutingId);

    const current = this.peersByRid.get(descriptor.nodeRoutingId);
    if (
      current !== undefined
      && current.descriptor.lifecycleGeneration === descriptor.lifecycleGeneration
      && descriptor.descriptorRevision > current.descriptor.descriptorRevision
      && !sameImmutableDescriptor(current.descriptor, descriptor)
    ) {
      return 'invalidDescriptor';
    }
    if (
      current !== undefined
      && current.descriptor.lifecycleGeneration === descriptor.lifecycleGeneration
      && (
        current.descriptor.descriptorRevision > descriptor.descriptorRevision
        || (
          current.descriptor.descriptorRevision === descriptor.descriptorRevision
          && !sameDescriptor(current.descriptor, descriptor)
        )
      )
    ) {
      return 'staleDescriptor';
    }
    if (
      current !== undefined
      && current.descriptor.lifecycleGeneration === descriptor.lifecycleGeneration
      && current.descriptor.descriptorRevision === descriptor.descriptorRevision
      && sameDescriptor(current.descriptor, descriptor)
      && current.connectionId !== connectionId
      && compareConnectionCandidate(
        current.connectionDiscriminator,
        current.connectionId,
        connectionDiscriminator,
        connectionId
      ) <= 0
    ) {
      // Both manual directions can reach admission. The same comparison on
      // every retry keeps one physical candidate instead of replacing it in
      // arrival order.
      return 'staleDescriptor';
    }
    this.peersByRid.set(descriptor.nodeRoutingId, {
      descriptor: cloneDescriptor(descriptor),
      connectionId,
      connectionDiscriminator
    });
    this.remember(descriptor);
    this.rebuildSelections();
    return 'admitted';
  }

  disconnect(nodeRoutingId: string, connectionId: string): boolean {
    const current = this.peersByRid.get(nodeRoutingId);
    if (current === undefined || current.connectionId !== connectionId) return false;
    this.peersByRid.delete(nodeRoutingId);
    this.rebuildSelections();
    return true;
  }

  peer(nodeRoutingId: string): AdmittedServicePeer | undefined {
    const peer = this.peersByRid.get(nodeRoutingId);
    return peer === undefined ? undefined : clonePeer(peer);
  }

  peers(): readonly AdmittedServicePeer[] {
    return [...this.peersByRid.values()]
      .sort((left, right) => left.descriptor.nodeRoutingId.localeCompare(right.descriptor.nodeRoutingId))
      .map(clonePeer);
  }

  notRequiredPeers(): readonly ServiceNodeDescriptor[] {
    return [...this.notRequiredByRid.values()]
      .sort((left, right) => left.nodeRoutingId.localeCompare(right.nodeRoutingId))
      .map(cloneDescriptor);
  }

  replaceDiscoveredNotRequired(
    descriptors: readonly ServiceNodeDescriptor[]
  ): void {
    const next = new Map<string, ServiceNodeDescriptor>();
    for (const descriptor of descriptors) {
      validateDescriptor(descriptor);
      if (
        descriptor.meshName === this.local.meshName
        && descriptor.nodeRoutingId !== this.local.nodeRoutingId
        && descriptorConnectionNotRequired(this.local, descriptor)
      ) {
        next.set(descriptor.nodeRoutingId, cloneDescriptor(descriptor));
      }
    }
    this.notRequiredByRid.clear();
    for (const [nodeRoutingId, descriptor] of next) {
      this.notRequiredByRid.set(nodeRoutingId, descriptor);
      this.remember(descriptor);
    }
    this.rebuildSelections();
  }

  markNotRequired(descriptor: ServiceNodeDescriptor): void {
    validateDescriptor(descriptor);
    if (
      descriptor.meshName !== this.local.meshName
      || descriptor.nodeRoutingId === this.local.nodeRoutingId
      || !descriptorConnectionNotRequired(this.local, descriptor)
    ) {
      return;
    }
    this.peersByRid.delete(descriptor.nodeRoutingId);
    this.remember(descriptor);
    this.notRequiredByRid.set(descriptor.nodeRoutingId, cloneDescriptor(descriptor));
    this.rebuildSelections();
  }

  forgetNotRequired(nodeRoutingId: string): void {
    if (this.notRequiredByRid.delete(nodeRoutingId)) this.rebuildSelections();
  }

  knownDescriptor(nodeRoutingId: string): ServiceNodeDescriptor | undefined {
    const admitted = this.peersByRid.get(nodeRoutingId)?.descriptor;
    const descriptor = admitted
      ?? this.notRequiredByRid.get(nodeRoutingId)
      ?? this.knownByRid.get(nodeRoutingId);
    return descriptor === undefined ? undefined : cloneDescriptor(descriptor);
  }

  private remember(descriptor: ServiceNodeDescriptor): void {
    const current = this.knownByRid.get(descriptor.nodeRoutingId);
    if (
      current === undefined
      || descriptor.lifecycleGeneration !== current.lifecycleGeneration
      || (
        descriptor.lifecycleGeneration === current.lifecycleGeneration
        && descriptor.descriptorRevision >= current.descriptorRevision
      )
    ) {
      this.knownByRid.set(descriptor.nodeRoutingId, cloneDescriptor(descriptor));
    }
  }

  selectChannel(
    channelName: string,
    isReady: (peer: AdmittedServicePeer) => boolean = () => true
  ): AdmittedServicePeer | undefined {
    requireText(channelName, 'channelName');
    return this.selectWeightedCycle(
      `channel:${channelName}`,
      () => {
        const local: AdmittedServicePeer = {
          descriptor: cloneDescriptor(this.local),
          connectionId: 'local',
          connectionDiscriminator: 'local'
        };
        return [local, ...this.peersByRid.values()].map(peer => ({
          peer: clonePeer(peer),
          channel: findChannel(peer.descriptor, channelName)
        })).filter((value): value is {
          peer: AdmittedServicePeer;
          channel: ServiceChannelDescriptor;
        } => value.channel !== undefined
          && value.peer.descriptor.state === 'serving'
          && value.channel.weight > 0);
      },
      value => value.channel.weight,
      value => value.peer.descriptor.nodeRoutingId,
      (left, right) => left.peer.descriptor.nodeRoutingId.localeCompare(right.peer.descriptor.nodeRoutingId),
      value => isReady(value.peer)
    )?.peer;
  }

  selectPlacement(
    isReady: (peer: AdmittedServicePeer) => boolean = () => true
  ): AdmittedServicePeer | undefined {
    return this.selectWeightedCycle(
      'placement',
      () => [...this.peersByRid.values()].filter(peer => {
        const descriptor = peer.descriptor;
        return descriptor.state === 'serving'
          && descriptor.objectRole === 'server'
          && descriptor.placementWeight > 0
          && descriptor.activeCapacityUsed < descriptor.activeCapacityLimit
          && descriptor.pendingCapacityUsed < descriptor.pendingCapacityLimit;
      }).map(clonePeer),
      peer => peer.descriptor.placementWeight,
      peer => peer.descriptor.nodeRoutingId,
      (left, right) => left.descriptor.nodeRoutingId.localeCompare(right.descriptor.nodeRoutingId),
      isReady
    );
  }

  selectObjectPlacement(
    stableType: string,
    isReady: (descriptor: ServiceNodeDescriptor) => boolean = () => true
  ): ServiceNodeDescriptor | undefined {
    requireText(stableType, 'stableType');
    return this.selectWeightedCycle(
      `object:${stableType}`,
      () => {
        const capability = `object-type:${stableType}`;
        return [this.local, ...this.peersByRid.values()].map(value =>
          'descriptor' in value ? value.descriptor : value
        ).filter(descriptor =>
          descriptor.state === 'serving'
          && descriptor.objectRole === 'server'
          && descriptor.placementWeight > 0
          && descriptor.activeCapacityUsed < descriptor.activeCapacityLimit
          && descriptor.pendingCapacityUsed < descriptor.pendingCapacityLimit
          && descriptor.protocolCapabilities.includes(capability)
        ).map(cloneDescriptor);
      },
      descriptor => descriptor.placementWeight,
      descriptor => descriptor.nodeRoutingId,
      (left, right) => left.nodeRoutingId.localeCompare(right.nodeRoutingId),
      isReady
    );
  }

  objectPlacementStatus(
    stableType: string,
    isReady: (descriptor: ServiceNodeDescriptor) => boolean = () => true
  ): ServiceObjectPlacementStatus {
    requireText(stableType, 'stableType');
    const capability = `object-type:${stableType}`;
    const supported = [this.local, ...[...this.peersByRid.values()].map(peer => peer.descriptor)]
      .filter(descriptor =>
        descriptor.state === 'serving'
        && descriptor.objectRole === 'server'
        && descriptor.protocolCapabilities.includes(capability)
      );
    if (supported.length === 0) return 'unsupported';
    const withCapacity = supported.filter(descriptor =>
      descriptor.activeCapacityUsed < descriptor.activeCapacityLimit
      && descriptor.pendingCapacityUsed < descriptor.pendingCapacityLimit
    );
    if (withCapacity.length === 0) return 'capacity';
    const weighted = withCapacity.filter(descriptor => descriptor.placementWeight > 0);
    if (weighted.length === 0) return 'unsupported';
    return weighted.some(isReady) ? 'available' : 'unavailable';
  }

  instanceSpotPlacementTypes(): readonly string[] {
    const prefix = 'instance-spot-type:';
    return [...new Set([
      this.local,
      ...this.peers().map(peer => peer.descriptor)
    ]
      .filter(descriptor =>
        descriptor.state === 'serving'
        && descriptor.objectRole === 'server')
      .flatMap(descriptor => descriptor.protocolCapabilities)
      .filter(capability => capability.startsWith(prefix))
      .map(capability => capability.slice(prefix.length))
      .filter(stableType => stableType.length > 0))]
      .sort();
  }

  private selectWeightedCycle<T>(
    key: string,
    eligible: () => readonly T[],
    weight: (value: T) => number,
    identity: (value: T) => string,
    compare: (left: T, right: T) => number,
    accept: (value: T) => boolean = () => true
  ): T | undefined {
    let entry = this.selections.get(key);
    if (entry === undefined) {
      const selection = new SmoothWeightedSelection<unknown>(() => eligible()
        .map(value => ({
          value,
          id: identity(value),
          weight: weight(value)
        }))
        .sort((left, right) => compare(left.value as T, right.value as T)));
      entry = { selection };
      this.selections.set(key, entry);
    }
    return entry.selection.select(value => accept(value as T)) as T | undefined;
  }

  private rebuildSelections(): void {
    for (const entry of this.selections.values()) entry.selection.rebuild();
  }
}

export function validateDescriptor(descriptor: ServiceNodeDescriptor): void {
  requireText(descriptor.meshName, 'meshName');
  requireText(descriptor.nodeRoutingId, 'nodeRoutingId');
  requireText(descriptor.advertisedEndpoint, 'advertisedEndpoint');
  requireText(descriptor.securityIdentity, 'securityIdentity');
  if (descriptor.lifecycleGeneration <= 0n || descriptor.descriptorRevision <= 0n) {
    throw new RangeError('Lifecycle generation and descriptor revision must be non-zero.');
  }
  if (
    !Number.isSafeInteger(descriptor.effectiveMaxMessageBytes)
    || descriptor.effectiveMaxMessageBytes <= 0
    || descriptor.applicationVersion < 0n
  ) {
    throw new RangeError('Descriptor message bound or application version is invalid.');
  }
  validatePublicWeight(descriptor.placementWeight, 'placementWeight');
  validateCapacity(descriptor.activeCapacityLimit, false, 'activeCapacityLimit');
  validateCapacity(descriptor.pendingCapacityLimit, true, 'pendingCapacityLimit');
  validateCapacity(descriptor.activeCapacityUsed, true, 'activeCapacityUsed');
  validateCapacity(descriptor.pendingCapacityUsed, true, 'pendingCapacityUsed');
  if (
    descriptor.activeCapacityUsed > descriptor.activeCapacityLimit
    || descriptor.pendingCapacityUsed > descriptor.pendingCapacityLimit
  ) {
    throw new RangeError('Descriptor capacity use exceeds its limit.');
  }
  validateSortedUnique(
    descriptor.channels,
    channel => {
      requireText(channel.name, 'channel.name');
      validatePublicWeight(channel.weight, 'channel.weight');
      return channel.name;
    },
    'channels'
  );
  validateSortedUnique(
    descriptor.protocolCapabilities,
    capability => {
      requireText(capability, 'protocol capability');
      return capability;
    },
    'protocolCapabilities'
  );
  if (!descriptor.protocolCapabilities.includes(REQUIRED_CAPABILITY)) {
    throw new TypeError(`Descriptor requires capability '${REQUIRED_CAPABILITY}'.`);
  }
}

function validateCapacity(value: number, allowZero: boolean, field: string): void {
  if (!Number.isInteger(value) || value < (allowZero ? 0 : 1) || value > MAX_CAPACITY) {
    throw new RangeError(`${field} is outside its supported range.`);
  }
}

function validatePublicWeight(value: number, field: string): void {
  if (!Number.isInteger(value) || value < 0 || value > MAX_PUBLIC_WEIGHT) {
    throw new RangeError(`${field} must be an integer in 0..10000.`);
  }
}

function validateSortedUnique<T>(
  values: readonly T[],
  key: (value: T) => string,
  field: string
): void {
  let previous: string | undefined;
  for (const value of values) {
    const current = key(value);
    if (previous !== undefined && previous >= current) {
      throw new TypeError(`${field} must be sorted and unique.`);
    }
    previous = current;
  }
}

function findChannel(
  descriptor: ServiceNodeDescriptor,
  channelName: string
): ServiceChannelDescriptor | undefined {
  return descriptor.channels.find(channel => channel.name === channelName);
}

function requireText(value: string, field: string): void {
  if (value.length === 0 || value.includes('\0')) {
    throw new TypeError(`${field} must be non-empty text without NUL.`);
  }
}

function cloneDescriptor(descriptor: ServiceNodeDescriptor): ServiceNodeDescriptor {
  return {
    ...descriptor,
    channels: descriptor.channels.map(channel => ({ ...channel })),
    protocolCapabilities: [...descriptor.protocolCapabilities]
  };
}

function clonePeer(peer: AdmittedServicePeer): AdmittedServicePeer {
  return {
    descriptor: cloneDescriptor(peer.descriptor),
    connectionId: peer.connectionId,
    connectionDiscriminator: peer.connectionDiscriminator
  };
}

function sameDescriptor(left: ServiceNodeDescriptor, right: ServiceNodeDescriptor): boolean {
  return JSON.stringify(toComparable(left)) === JSON.stringify(toComparable(right));
}

function sameImmutableDescriptor(
  left: ServiceNodeDescriptor,
  right: ServiceNodeDescriptor
): boolean {
  return left.advertisedEndpoint === right.advertisedEndpoint
    && left.securityIdentity === right.securityIdentity
    && left.effectiveMaxMessageBytes === right.effectiveMaxMessageBytes
    && left.applicationVersion === right.applicationVersion
    && arraysEqual(left.protocolCapabilities, right.protocolCapabilities)
    && left.objectRole === right.objectRole
    && left.activeCapacityLimit === right.activeCapacityLimit
    && left.pendingCapacityLimit === right.pendingCapacityLimit
    && left.channels.length === right.channels.length
    && left.channels.every((channel, index) =>
      channel.name === right.channels[index]?.name);
}

function arraysEqual(left: readonly string[], right: readonly string[]): boolean {
  return left.length === right.length
    && left.every((value, index) => value === right[index]);
}

function compareConnectionCandidate(
  leftDiscriminator: string,
  leftId: string,
  rightDiscriminator: string,
  rightId: string
): number {
  const discriminator = leftDiscriminator.localeCompare(rightDiscriminator);
  return discriminator === 0 ? leftId.localeCompare(rightId) : discriminator;
}

function toComparable(descriptor: ServiceNodeDescriptor): unknown {
  return {
    ...descriptor,
    lifecycleGeneration: descriptor.lifecycleGeneration.toString(),
    descriptorRevision: descriptor.descriptorRevision.toString(),
    applicationVersion: descriptor.applicationVersion.toString()
  };
}
