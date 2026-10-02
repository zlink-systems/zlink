import type {
  ZLinkClientServerStatus,
  ZLinkClientServerRuntime,
  ZLinkClientServerTargetStatus,
  ZLinkFanoutStatus,
  ZLinkFanoutRuntime,
  ZLinkObservedStatus,
  ZLinkPeerStatus
} from '../../contracts';
import {
  ZLinkFrameworkRuntimeState,
  ZLinkPeerState,
  ZLinkTopologyReason,
  ZLinkTopologyState
} from '../../contracts';
import { ZLinkConfigurationException } from '../configuration';
import type { ZLinkChannelRuntimeManager } from '../channels/channel-runtime-manager';
import {
  RuntimeEventQueue,
  ZLINK_DEFAULT_TERMINAL_OBSERVATION_CAPACITY
} from './runtime-observation-queue';
import {
  runtimeStateIsReady,
  topologyRuntimeIsReady,
  topologyObservationIsTerminal
} from '../foundation/runtime-state-projections';
import type {
  ClientServerDescriptor,
  FanoutPublisherDescriptor
} from '../foundation/service-discovery-registry';

export { RuntimeEventQueue } from './runtime-observation-queue';

type RuntimeAccessor = () => ZLinkChannelRuntimeManager | undefined;
type HostStateAccessor = () => ZLinkFrameworkRuntimeState;
type TopologyStatus = ZLinkClientServerStatus | ZLinkFanoutStatus;
interface TopologySource<T extends TopologyStatus> {
  lastSnapshot?: T;
  readonly observers: Set<RuntimeEventQueue<T>>;
  stop?: () => void;
}

class TopologyStatusSources<T extends TopologyStatus> {
  private readonly sources = new Map<string, TopologySource<T>>();

  constructor(
    private readonly read: (channelName: string, sequence: bigint) => T,
    private readonly subscribe: (channelName: string, changed: () => void) => () => void,
    private readonly equivalent: (left: T, right: T) => boolean
  ) {}

  snapshot(channelName: string): T {
    let source = this.sources.get(channelName);
    const current = source?.lastSnapshot;
    if (current !== undefined && topologyObservationIsTerminal(current.state)) return current;
    const snapshot = this.read(channelName, current?.sequence ?? 0n);
    if (source === undefined) {
      source = { observers: new Set() };
      this.sources.set(channelName, source);
    }
    const published = this.publish(channelName, source, snapshot);
    if (source.stop === undefined && !topologyObservationIsTerminal(published.state)) {
      source.stop = this.subscribe(channelName, () => this.snapshot(channelName));
    }
    return published;
  }

  observe(
    channelName: string,
    capacity: number,
    signal?: AbortSignal
  ): AsyncIterable<ZLinkObservedStatus<T>> {
    const queue = new RuntimeEventQueue<T>(capacity, signal);
    const snapshot = this.snapshot(channelName);
    const current = this.sources.get(channelName)!;
    current.observers.add(queue);
    queue.onClose(() => {
      current.observers.delete(queue);
    });
    this.deliver(queue, snapshot, channelName);
    return queue;
  }

  hostStateChanged(): void {
    for (const channelName of this.sources.keys()) this.snapshot(channelName);
  }

  stopObservers(): void {
    for (const [channelName, source] of this.sources) {
      if (
        source.lastSnapshot === undefined ||
        topologyObservationIsTerminal(source.lastSnapshot.state)
      )
        continue;
      this.publish(channelName, source, {
        ...source.lastSnapshot,
        state: ZLinkTopologyState.Stopped,
        isReady: false
      });
    }
  }

  private publish(channelName: string, source: TopologySource<T>, snapshot: T): T {
    const previous = source.lastSnapshot;
    if (previous !== undefined && this.equivalent(previous, snapshot)) return previous;
    source.lastSnapshot = {
      ...snapshot,
      sequence: (previous?.sequence ?? 0n) + 1n,
      observedAt: new Date()
    };
    for (const queue of source.observers) this.deliver(queue, source.lastSnapshot, channelName);
    if (topologyObservationIsTerminal(source.lastSnapshot.state)) {
      source.stop?.();
      source.stop = undefined;
    }
    return source.lastSnapshot;
  }

  private deliver(queue: RuntimeEventQueue<T>, snapshot: T, channelName: string): void {
    if (topologyObservationIsTerminal(snapshot.state)) queue.seal(snapshot, channelName);
    else queue.push(snapshot, channelName);
  }
}

function sameTopologyState(left: TopologyStatus, right: TopologyStatus): boolean {
  return left.state === right.state && left.isReady === right.isReady;
}

function samePeer(left: ZLinkPeerStatus, right: ZLinkPeerStatus): boolean {
  return (
    left.nodeRid === right.nodeRid &&
    left.state === right.state &&
    left.unavailableReason === right.unavailableReason
  );
}

function sameClientServerStatus(
  left: ZLinkClientServerStatus,
  right: ZLinkClientServerStatus
): boolean {
  return (
    sameTopologyState(left, right) &&
    left.localRole === right.localRole &&
    left.readyTargetCount === right.readyTargetCount &&
    left.targets.length === right.targets.length &&
    left.targets.every(
      (target, index) =>
        samePeer(target, right.targets[index]!) && target.weight === right.targets[index]!.weight
    )
  );
}

function sameFanoutStatus(left: ZLinkFanoutStatus, right: ZLinkFanoutStatus): boolean {
  return (
    sameTopologyState(left, right) &&
    left.readyPublisherCount === right.readyPublisherCount &&
    left.publishers.length === right.publishers.length &&
    left.publishers.every((publisher, index) => samePeer(publisher, right.publishers[index]!))
  );
}

export class ZLinkClientServerRuntimeProjection implements ZLinkClientServerRuntime {
  private readonly sources = new TopologyStatusSources<ZLinkClientServerStatus>(
    (channelName, sequence) => this.snapshotCore(channelName, sequence),
    (channelName, changed) =>
      this.requireRuntime().observeClientServerTopology(channelName, changed),
    sameClientServerStatus
  );

  constructor(
    private readonly runtime: RuntimeAccessor,
    private readonly hostState: HostStateAccessor = () => ZLinkFrameworkRuntimeState.Serving
  ) {}

  snapshot(channelName: string): ZLinkClientServerStatus {
    return this.sources.snapshot(channelName);
  }

  private snapshotCore(channelName: string, sequence: bigint): ZLinkClientServerStatus {
    const topology = this.requireRuntime().clientServerTopology(channelName);
    if (topology.localRole === undefined) {
      throw new ZLinkConfigurationException(
        `ClientServer channel '${channelName}' is not registered.`
      );
    }
    const targets = topology.descriptors.map((descriptor) =>
      peerStatusForAvailability(descriptor, ZLinkTopologyReason.NoReadyTarget)
    );
    const readyTargetCount = targets.filter(
      (target) => target.state === ZLinkPeerState.Ready && target.weight > 0
    ).length;
    const hostState = this.hostState();
    const isReady = topologyRuntimeIsReady(hostState, readyTargetCount);
    return {
      channelName,
      localRole: topology.localRole,
      state: isReady ? ZLinkTopologyState.Ready : topologyStateForHost(hostState),
      isReady,
      readyTargetCount,
      targets,
      sequence,
      observedAt: new Date()
    };
  }

  observe(
    channelName: string,
    capacity = ZLINK_DEFAULT_TERMINAL_OBSERVATION_CAPACITY,
    signal?: AbortSignal
  ): AsyncIterable<ZLinkObservedStatus<ZLinkClientServerStatus>> {
    return this.sources.observe(channelName, capacity, signal);
  }

  hostStateChanged(): void {
    this.sources.hostStateChanged();
  }

  stopObservers(): void {
    this.sources.stopObservers();
  }

  isReady(channelName: string): boolean {
    return this.snapshot(channelName).isReady;
  }

  private requireRuntime(): ZLinkChannelRuntimeManager {
    const runtime = this.runtime();
    if (runtime === undefined)
      throw new ZLinkConfigurationException('ClientServer runtime has not started.');
    return runtime;
  }
}

export class ZLinkFanoutRuntimeProjection implements ZLinkFanoutRuntime {
  private readonly sources = new TopologyStatusSources<ZLinkFanoutStatus>(
    (channelName, sequence) => this.snapshotCore(channelName, sequence),
    (channelName, changed) => this.requireRuntime().observeFanoutTopology(channelName, changed),
    sameFanoutStatus
  );

  constructor(
    private readonly runtime: RuntimeAccessor,
    private readonly hostState: HostStateAccessor = () => ZLinkFrameworkRuntimeState.Serving
  ) {}

  snapshot(channelName: string): ZLinkFanoutStatus {
    return this.sources.snapshot(channelName);
  }

  private snapshotCore(channelName: string, sequence: bigint): ZLinkFanoutStatus {
    const publishers = this.requireRuntime()
      .fanoutTopology(channelName)
      .descriptors.map((descriptor) =>
        peerStatusForAvailability(descriptor, ZLinkTopologyReason.NoReadyPeer)
      );
    const readyPublisherCount = publishers.filter(
      (publisher) => publisher.state === ZLinkPeerState.Ready
    ).length;
    const hostState = this.hostState();
    const hostReady = runtimeStateIsReady(hostState);
    return {
      channelName,
      state: hostReady
        ? readyPublisherCount > 0
          ? ZLinkTopologyState.Ready
          : ZLinkTopologyState.Degraded
        : topologyStateForHost(hostState),
      isReady: topologyRuntimeIsReady(hostState, readyPublisherCount),
      readyPublisherCount,
      publishers,
      sequence,
      observedAt: new Date()
    };
  }

  observe(
    channelName: string,
    capacity = ZLINK_DEFAULT_TERMINAL_OBSERVATION_CAPACITY,
    signal?: AbortSignal
  ): AsyncIterable<ZLinkObservedStatus<ZLinkFanoutStatus>> {
    return this.sources.observe(channelName, capacity, signal);
  }

  hostStateChanged(): void {
    this.sources.hostStateChanged();
  }

  stopObservers(): void {
    this.sources.stopObservers();
  }

  private requireRuntime(): ZLinkChannelRuntimeManager {
    const runtime = this.runtime();
    if (runtime === undefined)
      throw new ZLinkConfigurationException('Fanout runtime has not started.');
    return runtime;
  }
}

function peerStatusForAvailability(
  descriptor: Pick<ClientServerDescriptor, 'serverRoutingId' | 'weight' | 'state'>,
  unavailableReason: ZLinkTopologyReason.NoReadyTarget
): ZLinkClientServerTargetStatus;
function peerStatusForAvailability(
  descriptor: Pick<FanoutPublisherDescriptor, 'publisherRoutingId' | 'state'>,
  unavailableReason: ZLinkTopologyReason.NoReadyPeer
): ZLinkPeerStatus;
function peerStatusForAvailability(
  descriptor:
    | Pick<ClientServerDescriptor, 'serverRoutingId' | 'weight' | 'state'>
    | Pick<FanoutPublisherDescriptor, 'publisherRoutingId' | 'state'>,
  noReadyReason: ZLinkTopologyReason.NoReadyTarget | ZLinkTopologyReason.NoReadyPeer
): ZLinkClientServerTargetStatus | ZLinkPeerStatus {
  let state = ZLinkPeerState.NotConnected;
  let unavailableReason: ZLinkTopologyReason | undefined = noReadyReason;
  switch (descriptor.state) {
    case 'retiring':
      state = ZLinkPeerState.Draining;
      unavailableReason = ZLinkTopologyReason.Draining;
      break;
    case 'serving':
      state = ZLinkPeerState.Ready;
      unavailableReason = undefined;
      break;
    case 'preparing':
      state = ZLinkPeerState.Connecting;
      break;
  }
  return 'serverRoutingId' in descriptor
    ? { nodeRid: descriptor.serverRoutingId, weight: descriptor.weight, state, unavailableReason }
    : { nodeRid: descriptor.publisherRoutingId, state, unavailableReason };
}

function topologyStateForHost(state: ZLinkFrameworkRuntimeState): ZLinkTopologyState {
  switch (state) {
    case ZLinkFrameworkRuntimeState.Preparing:
      return ZLinkTopologyState.Starting;
    case ZLinkFrameworkRuntimeState.Relocating:
    case ZLinkFrameworkRuntimeState.Relocated:
    case ZLinkFrameworkRuntimeState.Draining:
      return ZLinkTopologyState.Stopping;
    case ZLinkFrameworkRuntimeState.Stopped:
      return ZLinkTopologyState.Stopped;
    case ZLinkFrameworkRuntimeState.Error:
      return ZLinkTopologyState.Failed;
    case ZLinkFrameworkRuntimeState.Serving:
      return ZLinkTopologyState.Degraded;
  }
}
