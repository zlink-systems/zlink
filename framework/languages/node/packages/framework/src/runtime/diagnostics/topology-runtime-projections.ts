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
    const hostReady = hostState === ZLinkFrameworkRuntimeState.Serving;
    return {
      channelName,
      localRole: topology.localRole,
      state: hostReady
        ? readyTargetCount > 0
          ? ZLinkTopologyState.Ready
          : ZLinkTopologyState.Degraded
        : topologyStateForHost(hostState),
      isReady: hostReady && readyTargetCount > 0,
      readyTargetCount,
      targets,
      sequence: this.sequence,
      observedAt: new Date()
    };
  }

  observe(
    channelName: string,
    capacity = 64,
    signal?: AbortSignal
  ): AsyncIterable<ZLinkObservedStatus<ZLinkClientServerStatus>> {
    const runtime = this.requireRuntime();
    const queue = new RuntimeEventQueue<ZLinkClientServerStatus>(capacity, signal);
    let lastSnapshot = this.snapshot(channelName);
    const stop = runtime.observeClientServerTopology(channelName, () => {
      this.sequence += 1n;
      lastSnapshot = this.snapshot(channelName);
      queue.push(lastSnapshot);
    });
    const hostObserver: HostObserver = {
      changed: () => {
        this.sequence += 1n;
        lastSnapshot = this.snapshot(channelName);
        queue.push(lastSnapshot);
      },
      stop: () => {
        this.sequence += 1n;
        let current = lastSnapshot;
        try {
          current = this.snapshot(channelName);
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
        });
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
    return this.snapshot(channelName).isReady;
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
    const hostReady = hostState === ZLinkFrameworkRuntimeState.Serving;
    return {
      channelName,
      state: hostReady
        ? readyPublisherCount > 0
          ? ZLinkTopologyState.Ready
          : ZLinkTopologyState.Degraded
        : topologyStateForHost(hostState),
      isReady: hostReady && readyPublisherCount > 0,
      readyPublisherCount,
      publishers,
      sequence: this.sequence,
      observedAt: new Date()
    };
  }

  observe(
    channelName: string,
    capacity = 64,
    signal?: AbortSignal
  ): AsyncIterable<ZLinkObservedStatus<ZLinkFanoutStatus>> {
    const runtime = this.requireRuntime();
    const queue = new RuntimeEventQueue<ZLinkFanoutStatus>(capacity, signal);
    let lastSnapshot = this.snapshot(channelName);
    const stop = runtime.observeFanoutTopology(channelName, () => {
      this.sequence += 1n;
      lastSnapshot = this.snapshot(channelName);
      queue.push(lastSnapshot);
    });
    const hostObserver: HostObserver = {
      changed: () => {
        this.sequence += 1n;
        lastSnapshot = this.snapshot(channelName);
        queue.push(lastSnapshot);
      },
      stop: () => {
        this.sequence += 1n;
        let current = lastSnapshot;
        try {
          current = this.snapshot(channelName);
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
        });
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

export class RuntimeEventQueue<T>
  implements AsyncIterable<ZLinkObservedStatus<T>>, AsyncIterator<ZLinkObservedStatus<T>> {
  private pending?: T;
  private terminal?: T;
  private readonly waiters: Array<
    ((result: IteratorResult<ZLinkObservedStatus<T>>) => void) | undefined
  > = [];
  private waitersHead = 0;
  private waitersCount = 0;
  private cleanup?: () => void;
  private coalescedCount = 0n;
  private discardedTerminalCount = 0n;
  private sealed = false;
  private closed = false;
  private completeAfterTerminal = false;

  constructor(capacity: number, signal?: AbortSignal) {
    if (!Number.isInteger(capacity) || capacity <= 0) throw new RangeError('Observer capacity must be positive.');
    signal?.addEventListener('abort', () => this.close(), { once: true });
  }

  [Symbol.asyncIterator](): AsyncIterator<ZLinkObservedStatus<T>> { return this; }

  next(): Promise<IteratorResult<ZLinkObservedStatus<T>>> {
    if (this.pending !== undefined) {
      const value = this.pending;
      this.pending = undefined;
      return Promise.resolve({ done: false, value: this.observed(value) });
    }
    if (this.terminal !== undefined) {
      const value = this.terminal;
      this.terminal = undefined;
      const result = Promise.resolve({ done: false, value: this.observed(value) });
      if (this.completeAfterTerminal) this.finishAfterTerminal();
      return result;
    }
    if (this.closed) return Promise.resolve({ done: true, value: undefined });
    return new Promise(resolve => {
      this.waiters.push(resolve);
      this.waitersCount += 1;
    });
  }

  return(): Promise<IteratorResult<ZLinkObservedStatus<T>>> {
    this.close();
    return Promise.resolve({ done: true, value: undefined });
  }

  onClose(cleanup: () => void): void {
    if (this.closed) cleanup();
    else this.cleanup = cleanup;
  }

  push(value: T): void {
    if (this.closed || this.sealed) return;
    const waiter = this.takeWaiter();
    if (waiter !== undefined) waiter({ done: false, value: this.observed(value) });
    else {
      if (this.pending !== undefined) this.coalescedCount = saturatingIncrement(this.coalescedCount);
      this.pending = value;
    }
  }

  seal(value: T): void {
    if (this.closed) return;
    if (this.sealed) {
      this.discardedTerminalCount = saturatingIncrement(this.discardedTerminalCount);
      return;
    }
    this.sealed = true;
    this.detachSource();
    const waiter = this.takeWaiter();
    if (waiter !== undefined) {
      waiter({ done: false, value: this.observed(value) });
    }
    else {
      this.terminal = value;
    }
  }

  complete(value: T): void {
    if (this.closed) return;
    if (this.sealed) {
      this.discardedTerminalCount = saturatingIncrement(this.discardedTerminalCount);
      return;
    }
    this.sealed = true;
    this.completeAfterTerminal = true;
    this.detachSource();
    const waiter = this.takeWaiter();
    if (waiter !== undefined) {
      waiter({ done: false, value: this.observed(value) });
      this.finishAfterTerminal();
    }
    else {
      this.terminal = value;
    }
  }

  close(): void {
    if (this.closed) return;
    this.closed = true;
    this.pending = undefined;
    this.terminal = undefined;
    this.detachSource();
    let waiter: ((result: IteratorResult<ZLinkObservedStatus<T>>) => void) | undefined;
    while ((waiter = this.takeWaiter()) !== undefined) {
      waiter({ done: true, value: undefined });
    }
  }

  private finishAfterTerminal(): void {
    this.completeAfterTerminal = false;
    this.closed = true;
    this.pending = undefined;
    this.terminal = undefined;
    this.detachSource();
    let waiter: ((result: IteratorResult<ZLinkObservedStatus<T>>) => void) | undefined;
    while ((waiter = this.takeWaiter()) !== undefined) {
      waiter({ done: true, value: undefined });
    }
  }

  private detachSource(): void {
    this.cleanup?.();
    this.cleanup = undefined;
  }

  private observed(status: T): ZLinkObservedStatus<T> {
    return {
      status,
      loss: {
        coalescedCount: this.coalescedCount,
        discardedTerminalCount: this.discardedTerminalCount
      }
    };
  }

  private takeWaiter():
    | ((result: IteratorResult<ZLinkObservedStatus<T>>) => void)
    | undefined {
    if (this.waitersCount === 0) return undefined;
    const waiter = this.waiters[this.waitersHead];
    this.waiters[this.waitersHead] = undefined;
    this.waitersHead += 1;
    this.waitersCount -= 1;
    if (this.waitersCount === 0) {
      this.waiters.length = 0;
      this.waitersHead = 0;
    } else if (this.waitersHead >= 1024 && this.waitersHead * 2 >= this.waiters.length) {
      this.waiters.splice(0, this.waitersHead);
      this.waitersHead = 0;
    }
    return waiter;
  }
}

const MAX_OBSERVATION_LOSS = 9_223_372_036_854_775_807n;

function saturatingIncrement(value: bigint): bigint {
  return value >= MAX_OBSERVATION_LOSS ? MAX_OBSERVATION_LOSS : value + 1n;
}
