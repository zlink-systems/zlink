import {
  type RoutingId,
  ZLinkFrameworkException,
  ZLinkFrameworkRuntimeState,
  ZLinkObjectRole,
  ZLinkPeerState,
  ZLinkTopologyReason,
  ZLinkTopologyState,
  type ZLinkMeshNodeDescriptor,
  type ZLinkObservedStatus,
  type ZLinkRouteMeshRuntime,
  type ZLinkRouteMeshStatus
} from '../../contracts';

import { ZLINK_DEFAULT_PUBLIC_WEIGHT } from '../../contracts/Configuration/RegistrationBuilderPolicy';
import { createDeadlineExceededError } from '../abort';
import type { ZLinkActivationAdmission } from '../activation-admission';
import type { ZLinkRuntimeAdmissionGate } from '../admission';
import type { ZLinkBackendMeshNode } from '../backend';
import type { ZLinkSpotNodeOptions } from '../configuration';
import { isRelocationDebugEnabled } from '../diagnostics';
import {
  RuntimeEventQueue,
  ZLINK_DEFAULT_TERMINAL_OBSERVATION_CAPACITY
} from '../diagnostics/runtime-observation-queue';
import { debugPendingWorkNames, isStructuralGuardEnabled } from '../execution/state-lane';
import {
  runtimeStateIsReady,
  topologyObservationIsTerminal,
  topologyRuntimeIsReady
} from '../foundation/runtime-state-projections';
import {
  MeshNodeRuntimeState,
  MeshPeerRuntimeState
} from '../foundation/service-runtime-contracts';
import {
  ZLinkFrameworkInternalErrorKind,
  createInternalFrameworkException
} from '../framework-errors-internal';
import { ZLinkOwnerCleanupError } from '../locations/runtime';
const DEFAULT_ROUTE_MESH_DRAIN_TIMEOUT_MS = 30_000;
const MAX_ROUTE_MESH_OBSERVATION_WAIT_MS = 1000;
const PLACEMENT_OBSERVATION_INTERVAL_MS = 100;

type ZLinkDrainForceReason =
  'deadline_exceeded' | 'drain_state_publish_failed' | 'owner_cleanup_failed' | 'teardown_failed';

type ZLinkMeshDrainResult =
  | { readonly kind: 'drained' }
  | {
      readonly kind: 'forceStopped';
      readonly reason: ZLinkDrainForceReason;
      readonly error?: unknown;
    };

export interface ZLinkRouteMeshRuntimeCoordinatorOptions {
  readonly meshNames: readonly string[];
  readonly meshOptions: ReadonlyMap<string, ZLinkSpotNodeOptions>;
  readonly meshNode: (meshName: string) => ZLinkBackendMeshNode | undefined;
  readonly meshNodeDescriptor?: (meshName: string) => ZLinkMeshNodeDescriptor | undefined;
  readonly localPlacementCounts?: (meshName: string) => ZLinkLocalPlacementCounts | undefined;
  readonly isLocationStoreHealthy?: () => boolean;
  readonly hostState?: () => ZLinkFrameworkRuntimeState;
  readonly admission: ZLinkRuntimeAdmissionGate;
  readonly activationAdmission?: ZLinkActivationAdmission;
  readonly publishRetiring: (meshName: string, signal: AbortSignal) => Promise<void>;
  readonly rollbackRetiring: (meshName: string, signal: AbortSignal) => Promise<void>;
  readonly publishDraining: (meshName: string, signal: AbortSignal) => Promise<void>;
  readonly publishHostDraining: (signal: AbortSignal) => Promise<void>;
  readonly drainResources: (
    meshName: string,
    signal: AbortSignal,
    stopStartingSignal?: AbortSignal
  ) => Promise<void>;
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
  deadline?: Date;
  operation?: Promise<ZLinkMeshDrainResult>;
  result?: ZLinkMeshDrainResult;
  readonly waiters: Array<(result: ZLinkMeshDrainResult) => void>;
  readonly observers: Set<RuntimeEventQueue<ZLinkRouteMeshStatus>>;
  lastSnapshot?: ZLinkRouteMeshStatus;
}

export class ZLinkRouteMeshRuntimeCoordinator implements ZLinkRouteMeshRuntime {
  private readonly states = new Map<string, ZLinkMeshDrainState>();
  private placementObserver?: ReturnType<typeof setInterval>;
  private hostOperation?: Promise<ZLinkMeshDrainResult>;
  private shutdownOperation?: Promise<ZLinkMeshDrainResult>;
  private hostRetiringPrepared = false;

  constructor(private readonly options: ZLinkRouteMeshRuntimeCoordinatorOptions) {
    for (const meshName of options.meshNames) {
      options.admission.register(meshName);
      this.states.set(meshName, {
        state: ZLinkTopologyState.Starting,
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
    const published = this.publishCurrentStatus(meshName, this.requireState(meshName));
    if (published === undefined) throw routeNotFound(meshName);
    return published;
  }

  private buildSnapshot(meshName: string, sequence: bigint): ZLinkRouteMeshStatus {
    const drain = this.requireState(meshName);
    const node = this.options.meshNode(meshName);
    if (node === undefined) throw routeNotFound(meshName);
    const status = node.status();
    const descriptor = this.options.meshNodeDescriptor?.(meshName);
    const localPlacementCounts = this.options.localPlacementCounts?.(meshName);
    // Active counts come only from this MeshNode's activation records (runtime monitoring §5);
    // before the object managers exist nothing is active on it.
    const populationCapacity =
      descriptor === undefined
        ? undefined
        : {
            ...descriptor.populationCapacity,
            actors: {
              ...descriptor.populationCapacity.actors,
              active: localPlacementCounts?.activeActorCount ?? 0
            },
            spots: {
              ...descriptor.populationCapacity.spots,
              active: localPlacementCounts?.activeSpotCount ?? 0
            }
          };
    const backendPeers = node.peers();
    const peerChannels = backendPeers.map((peer) =>
      peer.routingId === null
        ? { names: [] as readonly string[], weights: [] as readonly number[] }
        : node.peerChannels(peer.routingId, peer.lifecycleGeneration)
    );
    const peers = backendPeers.map((peer) => {
      const state = peerState(peer.state);
      return {
        nodeRid: (peer.routingId === null ? '' : String(peer.routingId)) as RoutingId,
        state,
        unavailableReason: peerUnavailableReason(state)
      };
    });
    const channels = Object.entries(this.options.meshOptions.get(meshName)?.meshChannels ?? {}).map(
      ([channelName, channel]) => {
        const readyMemberCount = BigInt(
          peerChannels.filter(
            (entry, index) =>
              peers[index]?.state === ZLinkPeerState.Ready &&
              entry.names.some(
                (name, channelIndex) =>
                  name === channelName && (entry.weights[channelIndex] ?? 0) > 0
              )
          ).length
        );
        const localWeight =
          descriptor?.channelWeights[channelName] ?? channel.weight ?? ZLINK_DEFAULT_PUBLIC_WEIGHT;
        const readyTargetCount =
          Number(readyMemberCount) + (channel.server === true && localWeight > 0 ? 1 : 0);
        return { channelName, isReady: readyTargetCount > 0, readyTargetCount };
      }
    );
    const backendTopologyState = backendState(status.state);
    const hasUnavailableRequiredPeer = peers.some(
      (peer) =>
        peer.state === ZLinkPeerState.Connecting || peer.state === ZLinkPeerState.NotConnected
    );
    const localTopologyState =
      drain.state === ZLinkTopologyState.Ready ? backendTopologyState : drain.state;
    const hostState = this.options.hostState?.() ?? ZLinkFrameworkRuntimeState.Serving;
    const hostTopologyState = topologyStateForHost(hostState, localTopologyState);
    const locationStoreHealthy = this.options.isLocationStoreHealthy?.() ?? true;
    const state =
      hostTopologyState === ZLinkTopologyState.Ready &&
      (hasUnavailableRequiredPeer || !locationStoreHealthy)
        ? ZLinkTopologyState.Degraded
        : hostTopologyState;
    const hostReady = runtimeStateIsReady(hostState);
    const objectRole = descriptor?.objectRole ?? ZLinkObjectRole.None;
    const placementWeight = descriptor?.placementWeight ?? 0;
    const capacityAvailable =
      descriptor !== undefined &&
      populationCapacity !== undefined &&
      (this.options.activationAdmission?.hasHeadroom(meshName) ?? true) &&
      (hasRemainingCapacity(populationCapacity.actors) ||
        hasRemainingCapacity(populationCapacity.spots));
    const placementAvailable =
      objectRole === ZLinkObjectRole.Server &&
      placementWeight > 0 &&
      capacityAvailable &&
      locationStoreHealthy &&
      hostReady &&
      localTopologyState === ZLinkTopologyState.Ready;
    const snapshot: ZLinkRouteMeshStatus = {
      meshName,
      state,
      isReady: topologyRuntimeIsReady(hostState, state === ZLinkTopologyState.Ready ? 1 : 0),
      readyPeerCount: peers.filter((peer) => peer.state === ZLinkPeerState.Ready).length,
      channels: hostReady ? channels : channels.map((channel) => ({ ...channel, isReady: false })),
      peers,
      placement: {
        isAvailable: placementAvailable,
        activeActorCount: populationCapacity?.actors.active ?? 0,
        activeSpotCount: populationCapacity?.spots.active ?? 0,
        unavailableReason: placementUnavailableReason(
          placementAvailable,
          hostState,
          locationStoreHealthy
        )
      },
      sequence,
      observedAt: new Date()
    };
    return snapshot;
  }

  observe(
    meshName: string,
    capacity = ZLINK_DEFAULT_TERMINAL_OBSERVATION_CAPACITY,
    signal?: AbortSignal
  ): AsyncIterable<ZLinkObservedStatus<ZLinkRouteMeshStatus>> {
    const state = this.requireState(meshName);
    if (!Number.isInteger(capacity) || capacity <= 0)
      throw new RangeError('Observer capacity must be positive.');
    const snapshot = this.snapshot(meshName);
    const queue = new RuntimeEventQueue<ZLinkRouteMeshStatus>(capacity, signal);
    state.observers.add(queue);
    this.startPlacementObserver();
    queue.onClose(() => {
      state.observers.delete(queue);
      this.stopPlacementObserverIfIdle();
    });
    this.publishStatus(meshName, queue, snapshot);
    return queue;
  }

  isReady(meshName: string): boolean {
    return this.snapshot(meshName).isReady;
  }

  hostStateChanged(): void {
    for (const [meshName, state] of this.states) this.publishCurrentStatus(meshName, state);
  }

  stopObservers(): void {
    for (const [meshName, state] of this.states) {
      const published = state.lastSnapshot;
      if (published !== undefined && topologyObservationIsTerminal(published.state)) continue;
      const current = published ?? this.publishCurrentStatus(meshName, state);
      if (current === undefined) {
        for (const observer of [...state.observers]) observer.close();
      } else {
        this.publishSnapshot(
          meshName,
          state,
          terminalSnapshot(current, ZLinkTopologyState.Stopped)
        );
      }
    }
  }

  drain(
    meshName: string,
    deadlineMs: number = DEFAULT_ROUTE_MESH_DRAIN_TIMEOUT_MS,
    signal?: AbortSignal
  ): Promise<ZLinkMeshDrainResult> {
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
    const operation =
      state.operation ??
      new Promise<ZLinkMeshDrainResult>((resolve) => state.waiters.push(resolve));
    return waitForOperation(operation, signal);
  }

  drainHost(
    deadlineMs: number = DEFAULT_ROUTE_MESH_DRAIN_TIMEOUT_MS,
    signal?: AbortSignal
  ): Promise<ZLinkMeshDrainResult> {
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

  shutdownHost(
    deadline: number | Date = 30_000,
    signal?: AbortSignal,
    remainingTimeoutMs?: number
  ): Promise<ZLinkMeshDrainResult> {
    if (typeof deadline === 'number' && (!Number.isFinite(deadline) || deadline <= 0)) {
      return Promise.reject(new TypeError('Shutdown deadlineMs must be greater than zero.'));
    }
    if (this.shutdownOperation === undefined) {
      const operation = this.performHostShutdown(deadline, remainingTimeoutMs);
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
      () =>
        deadline.abort(
          createDeadlineExceededError('Retire descriptor publication deadline exceeded.')
        ),
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
        () =>
          rollback.abort(
            createDeadlineExceededError('Retire descriptor rollback deadline exceeded.')
          ),
        Math.min(deadlineMs, MAX_ROUTE_MESH_OBSERVATION_WAIT_MS)
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
  async relocateHost(
    deadlineMs: number,
    stopStartingSignal?: AbortSignal
  ): Promise<ZLinkMeshDrainResult> {
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
      await Promise.all(
        entries.map(([meshName]) =>
          this.options.drainResources(meshName, deadline.signal, stopStartingSignal)
        )
      );
      this.hostRetiringPrepared = false;
      return { kind: 'drained' };
    } catch (error) {
      const classified = drainFailureReason(error);
      if (isRelocationDebugEnabled()) {
        console.error('[zlink.runtime.relocation.drain_failed]', classified, error);
      }
      const reason: ZLinkDrainForceReason = deadline.signal.aborted
        ? 'deadline_exceeded'
        : classified;
      const rollback = new AbortController();
      const rollbackTimer = setTimeout(
        () =>
          rollback.abort(
            createDeadlineExceededError('Relocation descriptor rollback deadline exceeded.')
          ),
        Math.min(Math.max(1, deadlineMs), MAX_ROUTE_MESH_OBSERVATION_WAIT_MS)
      );
      try {
        await Promise.all(
          entries.map(([meshName]) => this.options.rollbackRetiring(meshName, rollback.signal))
        );
      } catch {
        throw new ZLinkRetiringRollbackError();
      } finally {
        clearTimeout(rollbackTimer);
        this.hostRetiringPrepared = false;
      }
      return { kind: 'forceStopped', reason, error };
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
    const timer = setTimeout(
      () => deadline.abort(createDeadlineExceededError('Drain deadline exceeded.')),
      deadlineMs
    );
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
      const reason: ZLinkDrainForceReason =
        classified !== 'teardown_failed'
          ? classified
          : deadline.signal.aborted
            ? 'deadline_exceeded'
            : classified;
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
    const timer = setTimeout(
      () => deadline.abort(createDeadlineExceededError('Drain deadline exceeded.')),
      deadlineMs
    );
    let result: ZLinkMeshDrainResult;
    try {
      if (!this.hostRetiringPrepared) {
        await Promise.all(
          entries.map(([meshName]) => this.options.publishRetiring(meshName, deadline.signal))
        );
      }
      this.hostRetiringPrepared = false;
      await Promise.all(
        entries.map(([meshName]) => this.options.drainResources(meshName, deadline.signal))
      );
      for (const [meshName, state] of entries) {
        this.options.admission.seal(meshName);
        this.transition(meshName, state, ZLinkTopologyState.Stopping);
      }
      await Promise.all(
        entries.map(([meshName]) => this.options.publishDraining(meshName, deadline.signal))
      );
      await this.options.publishHostDraining(deadline.signal);
      await Promise.all(
        entries.map(([meshName]) => this.options.admission.awaitZero(meshName, deadline.signal))
      );
      await this.options.cleanupHostResources(deadline.signal);
      result = { kind: 'drained' };
      for (const [meshName, state] of entries) {
        this.transition(meshName, state, ZLinkTopologyState.Stopped);
      }
    } catch (error) {
      const classified = drainFailureReason(error);
      const reason: ZLinkDrainForceReason =
        classified !== 'teardown_failed'
          ? classified
          : deadline.signal.aborted
            ? 'deadline_exceeded'
            : classified;
      await Promise.all(
        entries.map(([meshName]) =>
          this.options.forceStopResources(meshName).catch(() => undefined)
        )
      );
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

  private async performHostShutdown(
    cleanupDeadline: Date | number,
    remainingTimeoutMs?: number
  ): Promise<ZLinkMeshDrainResult> {
    const timeoutMs = Math.max(
      0,
      remainingTimeoutMs ??
        (cleanupDeadline instanceof Date ? cleanupDeadline.getTime() - Date.now() : cleanupDeadline)
    );
    const deadlineAt =
      cleanupDeadline instanceof Date ? cleanupDeadline : new Date(Date.now() + cleanupDeadline);
    const entries = [...this.states.entries()];
    if (entries.length === 0) return { kind: 'drained' };
    for (const [, state] of entries) {
      state.deadline = deadlineAt;
    }
    const deadline = new AbortController();
    const timer = setTimeout(
      () => deadline.abort(createDeadlineExceededError('Shutdown deadline exceeded.')),
      timeoutMs
    );
    let result: ZLinkMeshDrainResult;
    try {
      for (const [meshName, state] of entries) {
        this.options.admission.seal(meshName, ZLinkFrameworkInternalErrorKind.RuntimeShutdown);
        this.transition(meshName, state, ZLinkTopologyState.Stopping);
      }
      await Promise.all(
        entries.map(([meshName]) => this.options.publishDraining(meshName, deadline.signal))
      );
      await this.options.publishHostDraining(deadline.signal);
      await Promise.all(
        entries.map(([meshName]) => this.options.admission.awaitZero(meshName, deadline.signal))
      );
      await Promise.all(
        entries.map(([meshName]) => this.options.shutdownResources?.(meshName, deadline.signal))
      );
      await this.options.cleanupHostResources(deadline.signal);
      result = { kind: 'drained' };
      for (const [meshName, state] of entries) {
        this.transition(meshName, state, ZLinkTopologyState.Stopped);
      }
    } catch (error) {
      const classified = drainFailureReason(error);
      const reason: ZLinkDrainForceReason =
        classified !== 'teardown_failed'
          ? classified
          : deadline.signal.aborted
            ? 'deadline_exceeded'
            : classified;
      await Promise.all(
        entries.map(([meshName]) =>
          this.options.forceStopResources(meshName).catch(() => undefined)
        )
      );
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

  private transition(meshName: string, state: ZLinkMeshDrainState, next: ZLinkTopologyState): void {
    if (state.state === next) return;
    state.state = next;
    const published = this.publishCurrentStatus(meshName, state);
    if (topologyObservationIsTerminal(next) && published === undefined) {
      for (const observer of [...state.observers]) observer.close();
    }
  }

  private publishCurrentStatus(
    meshName: string,
    state: ZLinkMeshDrainState
  ): ZLinkRouteMeshStatus | undefined {
    const published = state.lastSnapshot;
    if (published !== undefined && topologyObservationIsTerminal(published.state)) return published;
    const desired = topologyObservationIsTerminal(state.state)
      ? state.state
      : topologyStateForHost(
          this.options.hostState?.() ?? ZLinkFrameworkRuntimeState.Serving,
          state.state
        );
    if (published !== undefined && topologyObservationIsTerminal(desired)) {
      return this.publishSnapshot(meshName, state, terminalSnapshot(published, desired));
    }
    if (this.options.meshNode(meshName) === undefined) return undefined;
    const current = this.buildSnapshot(meshName, published?.sequence ?? 0n);
    return this.publishSnapshot(
      meshName,
      state,
      topologyObservationIsTerminal(current.state)
        ? terminalSnapshot(current, current.state)
        : current
    );
  }

  private publishSnapshot(
    meshName: string,
    state: ZLinkMeshDrainState,
    current: ZLinkRouteMeshStatus
  ): ZLinkRouteMeshStatus {
    const published = state.lastSnapshot;
    if (published !== undefined && samePublicRouteMeshStatus(published, current)) return published;
    const next = { ...current, sequence: (published?.sequence ?? 0n) + 1n, observedAt: new Date() };
    state.lastSnapshot = next;
    for (const observer of [...state.observers]) this.publishStatus(meshName, observer, next);
    return next;
  }

  private publishStatus(
    meshName: string,
    observer: RuntimeEventQueue<ZLinkRouteMeshStatus>,
    status: ZLinkRouteMeshStatus
  ): void {
    if (topologyObservationIsTerminal(status.state)) {
      observer.seal(status, meshName);
    } else {
      observer.push(status, meshName);
    }
  }

  private startPlacementObserver(): void {
    if (this.placementObserver !== undefined) return;
    this.placementObserver = setInterval(
      () => this.observePlacementChanges(),
      PLACEMENT_OBSERVATION_INTERVAL_MS
    );
    this.placementObserver.unref();
  }

  private stopPlacementObserverIfIdle(): void {
    for (const state of this.states.values()) {
      if (state.observers.size > 0) return;
    }
    if (this.placementObserver !== undefined) clearInterval(this.placementObserver);
    this.placementObserver = undefined;
  }

  private observePlacementChanges(): void {
    for (const [meshName, state] of this.states) {
      this.publishCurrentStatus(meshName, state);
    }
  }

  private requireState(meshName: string): ZLinkMeshDrainState {
    const state = this.states.get(meshName);
    if (state !== undefined) return state;
    throw routeNotFound(meshName);
  }
}

function terminalSnapshot(
  current: ZLinkRouteMeshStatus,
  state: ZLinkTopologyState
): ZLinkRouteMeshStatus {
  return {
    ...current,
    state,
    isReady: false,
    channels: current.channels.map((channel) => ({ ...channel, isReady: false })),
    placement: {
      ...current.placement,
      isAvailable: false,
      unavailableReason: ZLinkTopologyReason.RuntimeNotReady
    }
  };
}

function samePublicRouteMeshStatus(
  left: ZLinkRouteMeshStatus,
  right: ZLinkRouteMeshStatus
): boolean {
  return (
    left.meshName === right.meshName &&
    left.state === right.state &&
    left.isReady === right.isReady &&
    left.readyPeerCount === right.readyPeerCount &&
    left.placement.isAvailable === right.placement.isAvailable &&
    left.placement.activeActorCount === right.placement.activeActorCount &&
    left.placement.activeSpotCount === right.placement.activeSpotCount &&
    left.placement.unavailableReason === right.placement.unavailableReason &&
    left.channels.length === right.channels.length &&
    left.channels.every((channel, index) => {
      const other = right.channels[index]!;
      return (
        channel.channelName === other.channelName &&
        channel.isReady === other.isReady &&
        channel.readyTargetCount === other.readyTargetCount
      );
    }) &&
    left.peers.length === right.peers.length &&
    left.peers.every((peer, index) => {
      const other = right.peers[index]!;
      return (
        peer.nodeRid === other.nodeRid &&
        peer.state === other.state &&
        peer.unavailableReason === other.unavailableReason
      );
    })
  );
}

function placementUnavailableReason(
  available: boolean,
  hostState: ZLinkFrameworkRuntimeState,
  locationStoreHealthy: boolean
): ZLinkTopologyReason | undefined {
  if (available) return undefined;
  switch (hostState) {
    case ZLinkFrameworkRuntimeState.Relocating:
    case ZLinkFrameworkRuntimeState.Relocated:
    case ZLinkFrameworkRuntimeState.Draining:
      return ZLinkTopologyReason.Draining;
    case ZLinkFrameworkRuntimeState.Serving:
      return !locationStoreHealthy
        ? ZLinkTopologyReason.LocationUnavailable
        : ZLinkTopologyReason.CapacityExceeded;
    default:
      return ZLinkTopologyReason.RuntimeNotReady;
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

function hasRemainingCapacity(capacity: {
  readonly active: number;
  readonly limit: number;
  readonly reserved?: number;
}): boolean {
  return capacity.limit === 0 || capacity.active + (capacity.reserved ?? 0) < capacity.limit;
}

export class ZLinkDrainingStatePublishError extends Error {
  constructor(cause: unknown) {
    super('Failed to publish draining peer rows.', { cause });
    this.name = 'ZLinkDrainingStatePublishError';
  }
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

function backendState(state: MeshNodeRuntimeState): ZLinkTopologyState {
  switch (state) {
    case MeshNodeRuntimeState.Preparing:
      return ZLinkTopologyState.Starting;
    case MeshNodeRuntimeState.Serving:
      return ZLinkTopologyState.Ready;
    case MeshNodeRuntimeState.Retiring:
    case MeshNodeRuntimeState.Draining:
      return ZLinkTopologyState.Stopping;
    case MeshNodeRuntimeState.Stopped:
      return ZLinkTopologyState.Stopped;
    default:
      return ZLinkTopologyState.Failed;
  }
}

function peerState(state: MeshPeerRuntimeState): ZLinkPeerState {
  switch (state) {
    case MeshPeerRuntimeState.Serving:
      return ZLinkPeerState.Ready;
    case MeshPeerRuntimeState.Draining:
      return ZLinkPeerState.Draining;
    case MeshPeerRuntimeState.NotRequired:
      return ZLinkPeerState.NotRequired;
    case MeshPeerRuntimeState.Connecting:
    case MeshPeerRuntimeState.Preparing:
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
  if (isStructuralGuardEnabled()) {
    const pending = debugPendingWorkNames();
    if (pending.length > 0) console.error('[zlink.runtime.drain.pending]', pending);
  }
  if (error instanceof ZLinkDrainingStatePublishError) return 'drain_state_publish_failed';
  if (
    error instanceof ZLinkOwnerCleanupError ||
    (error instanceof AggregateError &&
      error.errors.some((nested) => nested instanceof ZLinkOwnerCleanupError))
  ) {
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
