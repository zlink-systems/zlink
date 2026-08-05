import { ZLinkFrameworkInternalErrorKind, createInternalFrameworkException  } from '../framework-errors-internal';
import type { RoutingId } from '../../contracts';
import {
  ZLinkFrameworkException,
  ZLinkFrameworkRuntimeState,
  ZLinkTopologyState,
  ZLinkTopologyReason,
  ZLinkPeerState,
  type ZLinkObservedStatus,
  type ZLinkRouteMeshStatus,
  type ZLinkRouteMeshRuntime
} from '../../contracts';
import { RuntimeEventQueue } from '../diagnostics/topology-runtime-projections';
import { createDeadlineExceededError } from '../abort';

type ZLinkDrainForceReason =
  | 'deadline_exceeded'
  | 'drain_state_publish_failed'
  | 'owner_cleanup_failed'
  | 'teardown_failed';

type ZLinkMeshDrainResult =
  | { readonly kind: 'drained' }
  | { readonly kind: 'forceStopped'; readonly reason: ZLinkDrainForceReason };
import type { ZLinkBackendMeshNode } from '../backend';
import type { ZLinkRuntimeAdmissionGate } from '../admission';
import type { ZLinkSpotNodeOptions } from '../configuration';
import {
  ZLinkObjectRole,
  type ZLinkMeshNodeDescriptor
} from '../../contracts';

export interface ZLinkRouteMeshRuntimeCoordinatorOptions {
  readonly meshNames: readonly string[];
  readonly meshOptions: ReadonlyMap<string, ZLinkSpotNodeOptions>;
  readonly meshNode: (meshName: string) => ZLinkBackendMeshNode | undefined;
  readonly meshNodeDescriptor?: (
    meshName: string
  ) => ZLinkMeshNodeDescriptor | undefined;
  readonly localPlacementCounts?: (
    meshName: string
  ) => ZLinkLocalPlacementCounts | undefined;
  readonly isLocationStoreHealthy?: () => boolean;
  readonly hostState?: () => ZLinkFrameworkRuntimeState;
  readonly admission: ZLinkRuntimeAdmissionGate;
  readonly publishRetiring: (meshName: string, signal: AbortSignal) => Promise<void>;
  readonly rollbackRetiring: (meshName: string, signal: AbortSignal) => Promise<void>;
  readonly publishDraining: (meshName: string, signal: AbortSignal) => Promise<void>;
  readonly publishHostDraining: (signal: AbortSignal) => Promise<void>;
  readonly drainResources: (meshName: string, signal: AbortSignal) => Promise<void>;
  readonly shutdownResources?: (meshName: string, signal: AbortSignal) => Promise<void>;
  readonly cleanupHostResources: (signal: AbortSignal) => Promise<void>;
  readonly forceStopResources: (meshName: string) => Promise<void>;
}

export interface ZLinkLocalPlacementCounts {
  readonly activeActorCount: number;
  readonly activeSpotCount: number;
}

interface ZLinkMeshDrainState {
  state: ZLinkTopologyState;
  sequence: bigint;
  deadline?: Date;
  operation?: Promise<ZLinkMeshDrainResult>;
  result?: ZLinkMeshDrainResult;
  readonly waiters: Array<(result: ZLinkMeshDrainResult) => void>;
  readonly observers: Set<RuntimeEventQueue<ZLinkRouteMeshStatus>>;
  lastSnapshot?: ZLinkRouteMeshStatus;
}

export class ZLinkRouteMeshRuntimeCoordinator implements ZLinkRouteMeshRuntime {
  private readonly states = new Map<string, ZLinkMeshDrainState>();
  private readonly placementFingerprints = new Map<string, string>();
  private readonly peerFingerprints = new Map<string, Map<string, string>>();
  private readonly locationStoreHealthFingerprints = new Map<string, boolean>();
  private placementObserver?: ReturnType<typeof setInterval>;
  private hostOperation?: Promise<ZLinkMeshDrainResult>;
  private shutdownOperation?: Promise<ZLinkMeshDrainResult>;
  private hostRetiringPrepared = false;

  constructor(private readonly options: ZLinkRouteMeshRuntimeCoordinatorOptions) {
    for (const meshName of options.meshNames) {
      options.admission.register(meshName);
      this.states.set(meshName, {
        state: ZLinkTopologyState.Starting,
        sequence: 0n,
        waiters: [],
        observers: new Set()
      });
    }
  }

  markServing(): void {
    for (const [meshName, state] of this.states) {
      if (state.state !== ZLinkTopologyState.Starting) continue;
      this.transition(meshName, state, ZLinkTopologyState.Ready);
    }
  }

  snapshot(meshName: string): ZLinkRouteMeshStatus {
    const drain = this.requireState(meshName);
    const node = this.options.meshNode(meshName);
    if (node === undefined) throw routeNotFound(meshName);
    const status = node.status();
    const descriptor = this.options.meshNodeDescriptor?.(meshName);
    const localPlacementCounts = this.options.localPlacementCounts?.(meshName);
    const populationCapacity = descriptor === undefined || localPlacementCounts === undefined
      ? descriptor?.populationCapacity
      : {
          ...descriptor.populationCapacity,
          actors: {
            ...descriptor.populationCapacity.actors,
            active: localPlacementCounts.activeActorCount
          },
          spots: {
            ...descriptor.populationCapacity.spots,
            active: localPlacementCounts.activeSpotCount
          }
        };
    const backendPeers = node.peers();
    const peerChannels = backendPeers.map((peer) => peer.routingId === null
      ? { names: [] as readonly string[], weights: [] as readonly number[] }
      : node.peerChannels(peer.routingId, peer.lifecycleGeneration));
    const peers = backendPeers.map((peer) => {
      const state = peerState(peer.state);
      return {
        nodeRid: (peer.routingId === null ? '' : String(peer.routingId)) as RoutingId,
        state,
        unavailableReason: peerUnavailableReason(state)
      };
    });
    const channels = Object.entries(this.options.meshOptions.get(meshName)?.meshChannels ?? {})
      .map(([channelName, channel]) => {
        const readyMemberCount = BigInt(peerChannels.filter((entry, index) =>
          peers[index]?.state === ZLinkPeerState.Ready
          && entry.names.some((name, channelIndex) =>
            name === channelName && (entry.weights[channelIndex] ?? 0) > 0)
        ).length);
        const localWeight = descriptor?.channelWeights[channelName] ?? channel.weight ?? 100;
        const readyTargetCount = Number(readyMemberCount)
          + (channel.server === true && localWeight > 0 ? 1 : 0);
        return { channelName, isReady: readyTargetCount > 0, readyTargetCount };
      });
    const backendTopologyState = backendState(status.state);
    const hasUnavailableRequiredPeer = peers.some(peer =>
      peer.state === ZLinkPeerState.Connecting
      || peer.state === ZLinkPeerState.NotConnected);
    const localTopologyState = drain.state === ZLinkTopologyState.Ready
      ? backendTopologyState
      : drain.state;
    const hostState = this.options.hostState?.()
      ?? ZLinkFrameworkRuntimeState.Serving;
    const hostTopologyState = topologyStateForHost(hostState, localTopologyState);
    const locationStoreHealthy = this.options.isLocationStoreHealthy?.() ?? true;
    const state = hostTopologyState === ZLinkTopologyState.Ready
      && (hasUnavailableRequiredPeer || !locationStoreHealthy)
      ? ZLinkTopologyState.Degraded
      : hostTopologyState;
    const hostReady = hostState === ZLinkFrameworkRuntimeState.Serving;
    const objectRole = descriptor?.objectRole ?? ZLinkObjectRole.None;
    const placementWeight = descriptor?.placementWeight ?? 0;
    const capacityAvailable = descriptor !== undefined
      && populationCapacity !== undefined
      && hasRemainingCapacity(descriptor.activationConcurrency)
      && (
        hasRemainingCapacity(populationCapacity.actors)
        || hasRemainingCapacity(populationCapacity.spots)
      );
    const placementAvailable = objectRole === ZLinkObjectRole.Server
      && placementWeight > 0
      && capacityAvailable
      && locationStoreHealthy
      && hostReady
      && localTopologyState === ZLinkTopologyState.Ready;
    const snapshot: ZLinkRouteMeshStatus = {
      meshName,
      state,
      isReady: hostReady && state === ZLinkTopologyState.Ready,
      readyPeerCount: peers.filter(peer => peer.state === ZLinkPeerState.Ready).length,
      channels: hostReady
        ? channels
        : channels.map(channel => ({ ...channel, isReady: false })),
      peers,
      placement: {
        isAvailable: placementAvailable,
        activeActorCount: populationCapacity?.actors.active ?? 0,
        activeSpotCount: populationCapacity?.spots.active ?? 0,
        unavailableReason: placementAvailable
          ? undefined
          : localTopologyState !== ZLinkTopologyState.Ready
            ? ZLinkTopologyReason.RuntimeNotReady
            : !locationStoreHealthy
              ? ZLinkTopologyReason.LocationUnavailable
            : placementWeight <= 0
              ? ZLinkTopologyReason.NoReadyTarget
              : ZLinkTopologyReason.CapacityExceeded
      },
      sequence: drain.sequence > status.lastChangedMs ? drain.sequence : status.lastChangedMs,
      observedAt: new Date()
    };
    drain.lastSnapshot = snapshot;
    return snapshot;
  }

  observe(
    meshName: string,
    capacity = 64,
    signal?: AbortSignal
  ): AsyncIterable<ZLinkObservedStatus<ZLinkRouteMeshStatus>> {
    const state = this.requireState(meshName);
    if (!Number.isInteger(capacity) || capacity <= 0) throw new RangeError('Observer capacity must be positive.');
    if (state.observers.size === 0) this.seedPlacementFingerprint(meshName);
    const queue = new RuntimeEventQueue<ZLinkRouteMeshStatus>(capacity, signal);
    queue.onClose(() => {
      state.observers.delete(queue);
      this.stopPlacementObserverIfIdle();
    });
    this.snapshot(meshName);
    state.observers.add(queue);
    this.startPlacementObserver();
    return queue;
  }

  isReady(meshName: string): boolean {
    return this.snapshot(meshName).isReady;
  }

  hostStateChanged(): void {
    for (const [meshName, state] of this.states) {
      state.sequence += 1n;
      let snapshot: ZLinkRouteMeshStatus;
      try {
        snapshot = this.snapshot(meshName);
      } catch {
        continue;
      }
      for (const observer of state.observers) observer.push(snapshot);
    }
  }

  stopObservers(): void {
    for (const [meshName, state] of this.states) {
      state.sequence += 1n;
      let terminal: ZLinkRouteMeshStatus | undefined;
      try {
        const current = this.snapshot(meshName);
        const terminalSequence = current.sequence >= state.sequence
          ? current.sequence + 1n
          : state.sequence;
        state.sequence = terminalSequence;
        terminal = {
          ...current,
          state: ZLinkTopologyState.Stopped,
          isReady: false,
          channels: current.channels.map(channel => ({
            ...channel,
            isReady: false
          })),
          placement: {
            ...current.placement,
            isAvailable: false,
            unavailableReason: ZLinkTopologyReason.RuntimeNotReady
          },
          sequence: terminalSequence,
          observedAt: new Date()
        };
      } catch {
        const current = state.lastSnapshot;
        if (current !== undefined) {
          const terminalSequence = current.sequence >= state.sequence
            ? current.sequence + 1n
            : state.sequence;
          state.sequence = terminalSequence;
          terminal = {
            ...current,
            state: ZLinkTopologyState.Stopped,
            isReady: false,
            channels: current.channels.map(channel => ({
              ...channel,
              isReady: false
            })),
            placement: {
              ...current.placement,
              isAvailable: false,
              unavailableReason: ZLinkTopologyReason.RuntimeNotReady
            },
            sequence: terminalSequence,
            observedAt: new Date()
          };
        }
      }
      for (const observer of [...state.observers]) {
        if (terminal === undefined) observer.close();
        else observer.seal(terminal);
      }
      state.observers.clear();
    }
  }

  drain(meshName: string, deadlineMs = 30_000, signal?: AbortSignal): Promise<ZLinkMeshDrainResult> {
    const state = this.requireState(meshName);
    if (this.states.size > 1) return Promise.reject(multiMeshDrainError());
    if (!Number.isFinite(deadlineMs) || deadlineMs <= 0) {
      return Promise.reject(new TypeError('Drain deadlineMs must be greater than zero.'));
    }
    state.operation ??= this.performDrain(meshName, state, deadlineMs);
    return waitForOperation(state.operation, signal);
  }

  awaitDrained(meshName: string, signal?: AbortSignal): Promise<ZLinkMeshDrainResult> {
    const state = this.requireState(meshName);
    if (this.states.size > 1) return Promise.reject(multiMeshDrainError());
    const operation = state.operation ?? new Promise<ZLinkMeshDrainResult>((resolve) => state.waiters.push(resolve));
    return waitForOperation(operation, signal);
  }

  drainHost(deadlineMs = 30_000, signal?: AbortSignal): Promise<ZLinkMeshDrainResult> {
    if (!Number.isFinite(deadlineMs) || deadlineMs <= 0) {
      return Promise.reject(new TypeError('Drain deadlineMs must be greater than zero.'));
    }
    if (this.hostOperation === undefined) {
      const onlyState = this.states.size === 1 ? this.states.values().next().value : undefined;
      const operation = onlyState?.operation ?? this.performHostDrain(deadlineMs);
      this.hostOperation = operation;
      for (const state of this.states.values()) state.operation ??= operation;
    }
    return waitForOperation(this.hostOperation, signal);
  }

  shutdownHost(deadlineMs = 30_000, signal?: AbortSignal): Promise<ZLinkMeshDrainResult> {
    if (!Number.isFinite(deadlineMs) || deadlineMs <= 0) {
      return Promise.reject(new TypeError('Shutdown deadlineMs must be greater than zero.'));
    }
    if (this.shutdownOperation === undefined) {
      const operation = this.performHostShutdown(deadlineMs);
      this.shutdownOperation = operation;
      for (const state of this.states.values()) state.operation ??= operation;
    }
    return waitForOperation(this.shutdownOperation, signal);
  }

  async prepareHostRetire(
    deadlineMs: number
  ): Promise<'prepared' | 'store_unavailable' | 'deadline_exceeded'> {
    if (this.hostRetiringPrepared) return 'prepared';
    const deadline = new AbortController();
    const timer = setTimeout(
      () => deadline.abort(createDeadlineExceededError('Retire descriptor publication deadline exceeded.')),
      deadlineMs
    );
    const attempted: string[] = [];
    try {
      for (const meshName of this.states.keys()) {
        attempted.push(meshName);
        await this.options.publishRetiring(meshName, deadline.signal);
      }
      this.hostRetiringPrepared = true;
      return 'prepared';
    } catch {
      // A failed response can still follow a committed Store write. Restore
      // every attempted descriptor before the host reports a reversible block.
      const rollback = new AbortController();
      const rollbackTimer = setTimeout(
        () => rollback.abort(createDeadlineExceededError('Retire descriptor rollback deadline exceeded.')),
        Math.min(deadlineMs, 1000)
      );
      let rollbackFailed = false;
      for (const meshName of attempted.reverse()) {
        try {
          await this.options.rollbackRetiring(meshName, rollback.signal);
        } catch {
          rollbackFailed = true;
        }
      }
      clearTimeout(rollbackTimer);
      if (rollbackFailed) {
        throw new ZLinkRetiringRollbackError();
      }
      return deadline.signal.aborted ? 'deadline_exceeded' : 'store_unavailable';
    } finally {
      clearTimeout(timer);
    }
  }

  /**
   * Relocates stateful resources without closing application transport.
   * Shutdown owns admission sealing, peer/listener teardown and owner cleanup.
   */
  async relocateHost(deadlineMs: number): Promise<ZLinkMeshDrainResult> {
    if (!Number.isFinite(deadlineMs) || deadlineMs <= 0) {
      throw new TypeError('Relocation deadlineMs must be greater than zero.');
    }
    const entries = [...this.states.entries()];
    const deadline = new AbortController();
    const timer = setTimeout(
      () => deadline.abort(createDeadlineExceededError('Relocation deadline exceeded.')),
      deadlineMs
    );
    try {
      await Promise.all(entries.map(([meshName]) =>
        this.options.drainResources(meshName, deadline.signal)));
      this.hostRetiringPrepared = false;
      return { kind: 'drained' };
    } catch (error) {
      const classified = drainFailureReason(error);
      const reason: ZLinkDrainForceReason = deadline.signal.aborted
        ? 'deadline_exceeded'
        : classified;
      const rollback = new AbortController();
      const rollbackTimer = setTimeout(
        () => rollback.abort(createDeadlineExceededError('Relocation descriptor rollback deadline exceeded.')),
        Math.min(Math.max(1, deadlineMs), 1000)
      );
      try {
        await Promise.all(entries.map(([meshName]) =>
          this.options.rollbackRetiring(meshName, rollback.signal)));
      } catch {
        throw new ZLinkRetiringRollbackError();
      } finally {
        clearTimeout(rollbackTimer);
        this.hostRetiringPrepared = false;
      }
      return { kind: 'forceStopped', reason };
    } finally {
      clearTimeout(timer);
    }
  }

  private async performDrain(
    meshName: string,
    state: ZLinkMeshDrainState,
    deadlineMs: number
  ): Promise<ZLinkMeshDrainResult> {
    state.deadline = new Date(Date.now() + deadlineMs);
    this.options.admission.seal(meshName);
    this.transition(meshName, state, ZLinkTopologyState.Stopping);
    const deadline = new AbortController();
    const timer = setTimeout(() => deadline.abort(createDeadlineExceededError('Drain deadline exceeded.')), deadlineMs);
    let result: ZLinkMeshDrainResult;
    try {
      await this.options.publishDraining(meshName, deadline.signal);
      await this.options.publishHostDraining(deadline.signal);
      await this.options.admission.awaitZero(meshName, deadline.signal);
      await this.options.drainResources(meshName, deadline.signal);
      await this.options.cleanupHostResources(deadline.signal);
      result = { kind: 'drained' };
      this.transition(meshName, state, ZLinkTopologyState.Stopped);
    } catch (error) {
      const classified = drainFailureReason(error);
      const reason: ZLinkDrainForceReason = classified !== 'teardown_failed'
        ? classified
        : deadline.signal.aborted ? 'deadline_exceeded' : classified;
      await this.options.forceStopResources(meshName).catch(() => undefined);
      result = { kind: 'forceStopped', reason };
      this.transition(meshName, state, ZLinkTopologyState.Failed);
    } finally {
      clearTimeout(timer);
    }
    state.result = result;
    for (const resolve of state.waiters) resolve(result);
    state.waiters.length = 0;
    return result;
  }

  private async performHostDrain(deadlineMs: number): Promise<ZLinkMeshDrainResult> {
    const entries = [...this.states.entries()];
    if (entries.length === 0) return { kind: 'drained' };
    const deadlineAt = new Date(Date.now() + deadlineMs);
    for (const [, state] of entries) {
      state.deadline = deadlineAt;
    }
    const deadline = new AbortController();
    const timer = setTimeout(() => deadline.abort(createDeadlineExceededError('Drain deadline exceeded.')), deadlineMs);
    let result: ZLinkMeshDrainResult;
    try {
      if (!this.hostRetiringPrepared) {
        await Promise.all(entries.map(([meshName]) =>
          this.options.publishRetiring(meshName, deadline.signal)));
      }
      this.hostRetiringPrepared = false;
      await Promise.all(entries.map(([meshName]) =>
        this.options.drainResources(meshName, deadline.signal)));
      for (const [meshName, state] of entries) {
        this.options.admission.seal(meshName);
        this.transition(meshName, state, ZLinkTopologyState.Stopping);
      }
      await Promise.all(entries.map(([meshName]) =>
        this.options.publishDraining(meshName, deadline.signal)));
      await this.options.publishHostDraining(deadline.signal);
      await Promise.all(entries.map(([meshName]) =>
        this.options.admission.awaitZero(meshName, deadline.signal)));
      await this.options.cleanupHostResources(deadline.signal);
      result = { kind: 'drained' };
      for (const [meshName, state] of entries) {
        this.transition(meshName, state, ZLinkTopologyState.Stopped);
      }
    } catch (error) {
      const classified = drainFailureReason(error);
      const reason: ZLinkDrainForceReason = classified !== 'teardown_failed'
        ? classified
        : deadline.signal.aborted ? 'deadline_exceeded' : classified;
      await Promise.all(entries.map(([meshName]) =>
        this.options.forceStopResources(meshName).catch(() => undefined)));
      result = { kind: 'forceStopped', reason };
      for (const [meshName, state] of entries) {
        this.transition(meshName, state, ZLinkTopologyState.Failed);
      }
    } finally {
      clearTimeout(timer);
    }
    for (const [, state] of entries) {
      state.result = result;
      for (const resolve of state.waiters) resolve(result);
      state.waiters.length = 0;
    }
    return result;
  }

  private async performHostShutdown(deadlineMs: number): Promise<ZLinkMeshDrainResult> {
    const entries = [...this.states.entries()];
    if (entries.length === 0) return { kind: 'drained' };
    const deadlineAt = new Date(Date.now() + deadlineMs);
    for (const [, state] of entries) {
      state.deadline = deadlineAt;
    }
    const deadline = new AbortController();
    const timer = setTimeout(
      () => deadline.abort(createDeadlineExceededError('Shutdown deadline exceeded.')),
      deadlineMs
    );
    let result: ZLinkMeshDrainResult;
    try {
      for (const [meshName, state] of entries) {
        this.options.admission.seal(
          meshName,
          ZLinkFrameworkInternalErrorKind.RuntimeShutdown
        );
        this.transition(meshName, state, ZLinkTopologyState.Stopping);
      }
      await Promise.all(entries.map(([meshName]) =>
        this.options.publishDraining(meshName, deadline.signal)));
      await this.options.publishHostDraining(deadline.signal);
      await Promise.all(entries.map(([meshName]) =>
        this.options.admission.awaitZero(meshName, deadline.signal)));
      await Promise.all(entries.map(([meshName]) =>
        this.options.shutdownResources?.(meshName, deadline.signal)));
      await this.options.cleanupHostResources(deadline.signal);
      result = { kind: 'drained' };
      for (const [meshName, state] of entries) {
        this.transition(meshName, state, ZLinkTopologyState.Stopped);
      }
    } catch (error) {
      const classified = drainFailureReason(error);
      const reason: ZLinkDrainForceReason = classified !== 'teardown_failed'
        ? classified
        : deadline.signal.aborted ? 'deadline_exceeded' : classified;
      await Promise.all(entries.map(([meshName]) =>
        this.options.forceStopResources(meshName).catch(() => undefined)));
      result = { kind: 'forceStopped', reason };
      for (const [meshName, state] of entries) {
        this.transition(meshName, state, ZLinkTopologyState.Failed);
      }
    } finally {
      clearTimeout(timer);
    }
    for (const [, state] of entries) {
      state.result = result;
      for (const resolve of state.waiters) resolve(result);
      state.waiters.length = 0;
    }
    return result;
  }

  private transition(
    meshName: string,
    state: ZLinkMeshDrainState,
    next: ZLinkTopologyState
  ): void {
    if (state.state === next) return;
    state.state = next;
    state.sequence += 1n;
    let current: ZLinkRouteMeshStatus | undefined;
    let snapshotAvailable = false;
    try {
      current = this.snapshot(meshName);
      snapshotAvailable = true;
    } catch {
      current = state.lastSnapshot;
    }
    const terminal = next === ZLinkTopologyState.Stopped || next === ZLinkTopologyState.Failed;
    if (current !== undefined) {
      if (terminal) {
        const terminalSequence = current.sequence >= state.sequence
          ? current.sequence + 1n
          : state.sequence;
        state.sequence = terminalSequence;
        const terminalStatus = {
          ...current,
          state: next,
          isReady: false,
          channels: current.channels.map(channel => ({
            ...channel,
            isReady: false
          })),
          placement: {
            ...current.placement,
            isAvailable: false,
            unavailableReason: ZLinkTopologyReason.RuntimeNotReady
          },
          sequence: terminalSequence,
          observedAt: new Date()
        };
        for (const observer of state.observers) observer.seal(terminalStatus);
      } else if (snapshotAvailable) {
        for (const observer of state.observers) observer.push(current);
      }
    }
    if (terminal) {
      if (current === undefined) {
        for (const observer of state.observers) observer.close();
      }
      state.observers.clear();
      this.stopPlacementObserverIfIdle();
    }
  }

  private seedPlacementFingerprint(meshName: string): void {
    this.locationStoreHealthFingerprints.set(
      meshName,
      this.options.isLocationStoreHealthy?.() ?? true
    );
    const descriptor = this.options.meshNodeDescriptor?.(meshName);
    if (descriptor !== undefined) {
      this.placementFingerprints.set(meshName, placementFingerprint(descriptor));
    }
    this.peerFingerprints.set(meshName, peerFingerprintMap(
      this.options.meshNode(meshName)?.peers() ?? []
    ));
  }

  private startPlacementObserver(): void {
    if (this.placementObserver !== undefined) return;
    this.placementObserver = setInterval(() => this.observePlacementChanges(), 100);
    this.placementObserver.unref();
  }

  private stopPlacementObserverIfIdle(): void {
    if ([...this.states.values()].some((state) => state.observers.size > 0)) return;
    if (this.placementObserver !== undefined) clearInterval(this.placementObserver);
    this.placementObserver = undefined;
  }

  private observePlacementChanges(): void {
    for (const [meshName, state] of this.states) {
      if (state.observers.size === 0) continue;
      const locationStoreHealthy =
        this.options.isLocationStoreHealthy?.() ?? true;
      if (locationStoreHealthy !== this.locationStoreHealthFingerprints.get(meshName)) {
        this.locationStoreHealthFingerprints.set(meshName, locationStoreHealthy);
        state.sequence += 1n;
        for (const observer of state.observers) {
          observer.push(this.snapshot(meshName));
        }
      }
      const descriptor = this.options.meshNodeDescriptor?.(meshName);
      if (descriptor === undefined) continue;
      const fingerprint = placementFingerprint(descriptor);
      const previous = this.placementFingerprints.get(meshName);
      this.placementFingerprints.set(meshName, fingerprint);
      const node = this.options.meshNode(meshName);
      if (previous !== undefined && previous !== fingerprint) {
        state.sequence += 1n;
        for (const observer of state.observers) observer.push(this.snapshot(meshName));
      }

      const previousPeers = this.peerFingerprints.get(meshName) ?? new Map();
      const peers = node?.peers() ?? [];
      const nextPeers = peerFingerprintMap(peers);
      this.peerFingerprints.set(meshName, nextPeers);
      for (const peerRid of new Set([
        ...previousPeers.keys(),
        ...nextPeers.keys()
      ])) {
        if (previousPeers.get(peerRid) === nextPeers.get(peerRid)) continue;
        state.sequence += 1n;
        for (const observer of state.observers) observer.push(this.snapshot(meshName));
      }
    }
  }

  private requireState(meshName: string): ZLinkMeshDrainState {
    const state = this.states.get(meshName);
    if (state !== undefined) return state;
    throw routeNotFound(meshName);
  }
}

function topologyStateForHost(
  state: ZLinkFrameworkRuntimeState,
  fallback: ZLinkTopologyState
): ZLinkTopologyState {
  switch (state) {
    case ZLinkFrameworkRuntimeState.Serving:
      return fallback;
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
  }
}

function placementFingerprint(descriptor: ZLinkMeshNodeDescriptor): string {
  const spotTypes = [...descriptor.populationCapacity.spotTypes]
    .sort((left, right) => {
      const kind = left.objectKind.localeCompare(right.objectKind);
      return kind !== 0 ? kind : left.stableType.localeCompare(right.stableType);
    })
    .map((entry) => [
      entry.objectKind,
      entry.stableType,
      entry.active,
      entry.reserved,
      entry.limit
    ]);
  return JSON.stringify([
    descriptor.objectRole,
    descriptor.placementWeight,
    descriptor.populationCapacity.actors,
    descriptor.populationCapacity.spots,
    spotTypes,
    descriptor.activationConcurrency,
    Object.entries(descriptor.channelWeights).sort(([left], [right]) =>
      left.localeCompare(right))
  ]);
}

function hasRemainingCapacity(
  capacity: { readonly active: number; readonly limit: number; readonly reserved?: number }
): boolean {
  return capacity.limit === 0
    || capacity.active + (capacity.reserved ?? 0) < capacity.limit;
}

function peerFingerprintMap(
  peers: ReturnType<ZLinkBackendMeshNode['peers']>
): Map<string, string> {
  return new Map(peers.map(peer => [
    String(peer.routingId),
    JSON.stringify([
      peer.lifecycleGeneration.toString(),
      peer.descriptorRevision.toString(),
      peer.endpoint,
      peer.state,
      peer.lastError
    ])
  ]));
}

export class ZLinkRetiringRollbackError extends Error {
  constructor() {
    super('Retiring descriptor publication could not be rolled back to Serving.');
    this.name = 'ZLinkRetiringRollbackError';
  }
}

function routeNotFound(meshName: string): ZLinkFrameworkException {
  return createInternalFrameworkException(
    ZLinkFrameworkInternalErrorKind.RouteNotConnected,
    `RouteMesh '${meshName}' is not registered or no longer available.`
  );
}

function multiMeshDrainError(): ZLinkFrameworkException {
  return createInternalFrameworkException(
    ZLinkFrameworkInternalErrorKind.RequestRejected,
    'RouteMesh drain is unavailable when one framework host owns multiple RouteMesh instances.'
  );
}

function backendState(state: number): ZLinkTopologyState {
  switch (state) {
    case 1: return ZLinkTopologyState.Starting;
    case 2:
    case 3:
    case 4: return ZLinkTopologyState.Ready;
    case 5: return ZLinkTopologyState.Stopping;
    case 6: return ZLinkTopologyState.Stopped;
    default: return ZLinkTopologyState.Failed;
  }
}

function peerState(state: number): ZLinkPeerState {
  switch (state) {
    case 3: return ZLinkPeerState.Ready;
    case 4: return ZLinkPeerState.Draining;
    case 6: return ZLinkPeerState.NotRequired;
    case 1:
    case 2:
      return ZLinkPeerState.Connecting;
    default:
      return ZLinkPeerState.NotConnected;
  }
}

function peerUnavailableReason(state: ZLinkPeerState): ZLinkTopologyReason | undefined {
  switch (state) {
    case ZLinkPeerState.Ready:
    case ZLinkPeerState.NotRequired:
      return undefined;
    case ZLinkPeerState.Draining:
      return ZLinkTopologyReason.Draining;
    default:
      return ZLinkTopologyReason.NoReadyPeer;
  }
}

function drainFailureReason(error: unknown): ZLinkDrainForceReason {
  const name = error instanceof Error ? error.name : '';
  if (name === 'ZLinkDrainingStatePublishError') return 'drain_state_publish_failed';
  if (name === 'ZLinkOwnerCleanupError' || error instanceof AggregateError
      && error.errors.some((nested) => nested instanceof Error && nested.name === 'ZLinkOwnerCleanupError')) {
    return 'owner_cleanup_failed';
  }
  return 'teardown_failed';
}

function waitForOperation<T>(operation: Promise<T>, signal?: AbortSignal): Promise<T> {
  if (signal === undefined) return operation;
  if (signal.aborted) return Promise.reject(signal.reason);
  return new Promise<T>((resolve, reject) => {
    const abort = () => reject(signal.reason);
    signal.addEventListener('abort', abort, { once: true });
    operation.then(
      (result) => {
        signal.removeEventListener('abort', abort);
        resolve(result);
      },
      (error) => {
        signal.removeEventListener('abort', abort);
        reject(error);
      }
    );
  });
}
