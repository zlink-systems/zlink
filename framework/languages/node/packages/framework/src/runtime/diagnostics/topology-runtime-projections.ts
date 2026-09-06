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
  topologyRuntimeIsReady
} from '../foundation/runtime-state-projections';

export { RuntimeEventQueue } from './runtime-observation-queue';

type RuntimeAccessor = () => ZLinkChannelRuntimeManager | undefined;
type HostStateAccessor = () => ZLinkFrameworkRuntimeState;
interface HostObserver {
  readonly changed: () => void;
  readonly stop: () => void;
}

export class ZLinkClientServerRuntimeProjection implements ZLinkClientServerRuntime {
  private sequence = 0n;
  private readonly hostObservers = new Set<HostObserver>();

  constructor(
    private readonly runtime: RuntimeAccessor,
    private readonly hostState: HostStateAccessor =
      () => ZLinkFrameworkRuntimeState.Serving
  ) {}

  snapshot(channelName: string): ZLinkClientServerStatus {
    return this.snapshotCore(channelName);
  }

  private snapshotCore(channelName: string): ZLinkClientServerStatus {
    const topology = this.requireRuntime().clientServerTopology(channelName);
    if (topology.localRole === undefined) {
      throw new ZLinkConfigurationException(`ClientServer channel '${channelName}' is not registered.`);
    }
    const targets = topology.descriptors.map((descriptor): ZLinkClientServerTargetStatus => ({
      nodeRid: descriptor.serverRoutingId,
      weight: descriptor.weight,
      state: descriptor.state === 'serving' ? ZLinkPeerState.Ready
        : descriptor.state === 'retiring' ? ZLinkPeerState.Draining
          : descriptor.state === 'preparing' ? ZLinkPeerState.Connecting
            : ZLinkPeerState.NotConnected,
      unavailableReason: descriptor.state === 'serving' && descriptor.weight > 0
        ? undefined
        : descriptor.state === 'retiring'
          ? ZLinkTopologyReason.Draining
          : ZLinkTopologyReason.NoReadyTarget
    }));
    const readyTargetCount = targets.filter(
      target => target.state === ZLinkPeerState.Ready && target.weight > 0
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
      sequence: this.sequence,
      observedAt: new Date()
    };
  }

  observe(
    channelName: string,
    capacity = ZLINK_DEFAULT_TERMINAL_OBSERVATION_CAPACITY,
    signal?: AbortSignal
  ): AsyncIterable<ZLinkObservedStatus<ZLinkClientServerStatus>> {
    const runtime = this.requireRuntime();
    const queue = new RuntimeEventQueue<ZLinkClientServerStatus>(capacity, signal);
    let lastSnapshot = this.snapshotCore(channelName);
    const stop = runtime.observeClientServerTopology(channelName, () => {
      this.sequence += 1n;
      lastSnapshot = this.snapshotCore(channelName);
      queue.push(lastSnapshot, channelName);
    });
    const hostObserver: HostObserver = {
      changed: () => {
        this.sequence += 1n;
        lastSnapshot = this.snapshotCore(channelName);
        queue.push(lastSnapshot, channelName);
      },
      stop: () => {
        this.sequence += 1n;
        let current = lastSnapshot;
        try {
          current = this.snapshotCore(channelName);
          lastSnapshot = current;
        } catch {
          // The last complete projection remains valid after native teardown.
        }
        queue.seal({
          ...current,
          state: ZLinkTopologyState.Stopped,
          isReady: false,
          sequence: this.sequence,
          observedAt: new Date()
        }, channelName);
      }
    };
    this.hostObservers.add(hostObserver);
    queue.onClose(() => {
      stop();
      this.hostObservers.delete(hostObserver);
    });
    return queue;
  }

  hostStateChanged(): void {
    for (const observer of this.hostObservers) {
      try {
        observer.changed();
      } catch {
        // Monitoring projection failures do not change host lifecycle results.
      }
    }
  }

  stopObservers(): void {
    for (const observer of [...this.hostObservers]) {
      try {
        observer.stop();
      } catch {
        // The observer is still removed when its terminal snapshot fails.
      }
    }
    this.hostObservers.clear();
  }

  isReady(channelName: string): boolean {
    return this.snapshotCore(channelName).isReady;
  }

  private requireRuntime(): ZLinkChannelRuntimeManager {
    const runtime = this.runtime();
    if (runtime === undefined) throw new ZLinkConfigurationException('ClientServer runtime has not started.');
    return runtime;
  }
}

export class ZLinkFanoutRuntimeProjection implements ZLinkFanoutRuntime {
  private sequence = 0n;
  private readonly hostObservers = new Set<HostObserver>();

  constructor(
    private readonly runtime: RuntimeAccessor,
    private readonly hostState: HostStateAccessor =
      () => ZLinkFrameworkRuntimeState.Serving
  ) {}

  snapshot(channelName: string): ZLinkFanoutStatus {
    return this.snapshotCore(channelName);
  }

  private snapshotCore(channelName: string): ZLinkFanoutStatus {
    const publishers = this.requireRuntime().fanoutTopology(channelName).descriptors
      .map((descriptor): ZLinkPeerStatus => ({
        nodeRid: descriptor.publisherRoutingId,
        state: descriptor.state === 'serving' ? ZLinkPeerState.Ready
          : descriptor.state === 'retiring' ? ZLinkPeerState.Draining
            : descriptor.state === 'preparing' ? ZLinkPeerState.Connecting
              : ZLinkPeerState.NotConnected,
        unavailableReason: descriptor.state === 'serving'
          ? undefined
          : descriptor.state === 'retiring'
            ? ZLinkTopologyReason.Draining
            : ZLinkTopologyReason.NoReadyTarget
      }));
    const readyPublisherCount = publishers.filter(
      publisher => publisher.state === ZLinkPeerState.Ready
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
      sequence: this.sequence,
      observedAt: new Date()
    };
  }

  observe(
    channelName: string,
    capacity = ZLINK_DEFAULT_TERMINAL_OBSERVATION_CAPACITY,
    signal?: AbortSignal
  ): AsyncIterable<ZLinkObservedStatus<ZLinkFanoutStatus>> {
    const runtime = this.requireRuntime();
    const queue = new RuntimeEventQueue<ZLinkFanoutStatus>(capacity, signal);
    let lastSnapshot = this.snapshotCore(channelName);
    const stop = runtime.observeFanoutTopology(channelName, () => {
      this.sequence += 1n;
      lastSnapshot = this.snapshotCore(channelName);
      queue.push(lastSnapshot, channelName);
    });
    const hostObserver: HostObserver = {
      changed: () => {
        this.sequence += 1n;
        lastSnapshot = this.snapshotCore(channelName);
        queue.push(lastSnapshot, channelName);
      },
      stop: () => {
        this.sequence += 1n;
        let current = lastSnapshot;
        try {
          current = this.snapshotCore(channelName);
          lastSnapshot = current;
        } catch {
          // The last complete projection remains valid after native teardown.
        }
        queue.seal({
          ...current,
          state: ZLinkTopologyState.Stopped,
          isReady: false,
          sequence: this.sequence,
          observedAt: new Date()
        }, channelName);
      }
    };
    this.hostObservers.add(hostObserver);
    queue.onClose(() => {
      stop();
      this.hostObservers.delete(hostObserver);
    });
    return queue;
  }

  hostStateChanged(): void {
    for (const observer of this.hostObservers) {
      try {
        observer.changed();
      } catch {
        // Monitoring projection failures do not change host lifecycle results.
      }
    }
  }

  stopObservers(): void {
    for (const observer of [...this.hostObservers]) {
      try {
        observer.stop();
      } catch {
        // The observer is still removed when its terminal snapshot fails.
      }
    }
    this.hostObservers.clear();
  }

  private requireRuntime(): ZLinkChannelRuntimeManager {
    const runtime = this.runtime();
    if (runtime === undefined) throw new ZLinkConfigurationException('Fanout runtime has not started.');
    return runtime;
  }
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
