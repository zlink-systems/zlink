import {
  createInternalFrameworkException,
  ZLinkFrameworkInternalErrorKind
} from '../framework-errors-internal';
import { inspectRuntime } from '../internal-observation';
import {
  ZLinkDescriptorPublicationUnconfirmedError,
  ZLinkMeshNodeDescriptorConflictException
} from './descriptor-publication';
import { ZLinkOwnerLeaseReleaseDelayPendingError } from './owner-lease-release-delay';
import { isIndeterminateLocationStoreFailure } from './location-store-failure';
import { listAllMeshNodeDescriptors } from './location-store-pages';
import {
  ZLinkInternalMeshNodeWriteIntent,
  type ZLinkMeshNodeWriteIntent
} from './domain-store-contract';
import { ZLINK_PROVIDER_MAX_PAGE_SIZE } from '../../contracts/Locations/Stores';
import type { ZLinkAuthorityEntry, ZLinkAuthorityScanCursor } from './internal-location-contracts';
import { MILLISECONDS_PER_SECOND } from '../diagnostics/runtime-metrics';
import {
  zlinkRuntimeDefaultLocationOptions,
  type ZLinkLocationOptionOverrides
} from '../../contracts/Locations/Options';
import { normalizeEndpoint } from '../../contracts/Configuration/EndpointNotation';
import { randomUUID } from 'node:crypto';
import type { RoutingId } from '../../contracts/Common';
import {
  ZLinkLocationAutoConnectType,
  ZLinkFrameworkRuntimeState,
  ZLinkLocationKind,
  ZLinkLocationTopologyState,
  ZLinkLocationWriteIntent,
  ZLinkLocationWriteStatus,
  type ZLinkLocationRuntimeQuery,
  type ZLinkLocationOwnerToken,
  type ZLinkOwnerLeaseRenewed,
  type ZLinkActorLocation,
  type ZLinkActorLocationFilter,
  type ZLinkActorLocationKey,
  type ZLinkLocationKey,
  type ZLinkLocationPage,
  type ZLinkLocationRuntimeStatus,
  type ZLinkLocationServiceSummary,
  type ZLinkLocationServiceSummaryFilter,
  type ZLinkMeshNodeDescriptor,
  type ZLinkLocationTopologyEntry,
  type ZLinkLocationTopologyFilter,
  type ZLinkLocationWriteResult,
  type ZLinkAuthoritySnapshot,
  type ZLinkPageRequest,
  type ZLinkPeerLocation,
  type ZLinkPeerLocationFilter,
  type ZLinkPeerLocationKey,
  type ZLinkRouteLocation,
  type ZLinkRouteLocationFilter,
  type ZLinkRouteLocationKey,
  type ZLinkSpotLocation,
  type ZLinkSpotLocationFilter,
  type ZLinkSpotLocationKey
} from './internal-location-contracts';
import type {
  ZLinkLocationObjectEntry,
  ZLinkLocationObjectFilter
} from '../../contracts/Locations/RuntimeQuery';
import { ZLinkAuthorityScanCursor as PublicAuthorityScanCursor } from '../../contracts/Locations/Authority';
import { decodeAuthorityKey, encodeAuthorityKey } from './authority-key-codec';
import type {
  ZLinkActorLocationStore,
  ZLinkActorLocationQueryStore,
  ZLinkAuthorityStore,
  ZLinkClientServerLocationStore,
  ZLinkFanoutLocationStore,
  ZLinkOwnerLeaseStore,
  ZLinkPeerLocationStore,
  ZLinkRouteLocationStore,
  ZLinkSpotLocationStore,
  ZLinkSpotLocationQueryStore
} from './internal-store-contracts';
import type { ZLinkDomainLocationStore } from './domain-store-contract';
import { zlinkLocationAutoConnectTypeName, zlinkLocationRoleName } from './canonical-codec';
import { ZLinkLocationKeyCodec } from './key-codec';
import { ZLinkLiveRowFilter, ZLinkOwnerLeaseTracker } from './lease-tracker';
import { isAbortError } from '../abort';
import type {
  ZLinkExecutionRecovery,
  ZLinkOwnershipLostEvent,
  ZLinkPreservedAuthority,
  ZLinkAuthorityConfirmation
} from './lifecycle-runtime';

export interface ZLinkLocationRuntimeStores {
  readonly locationStore: ZLinkDomainLocationStore;
  readonly authorityStore: ZLinkAuthorityStore;
  readonly clientServerStore?: ZLinkClientServerLocationStore;
  readonly fanoutStore?: ZLinkFanoutLocationStore;
  readonly peerStore: ZLinkPeerLocationStore;
  readonly spotStore: ZLinkSpotLocationStore;
  readonly actorStore: ZLinkActorLocationStore;
  readonly routeStore: ZLinkRouteLocationStore;
  readonly ownerLeaseStore: ZLinkOwnerLeaseStore;
}

export interface ZLinkLocationRuntimeOptions {
  readonly stores: ZLinkLocationRuntimeStores;
  readonly options?: ZLinkLocationOptionOverrides;
  readonly ownerId?: string;
  readonly events?: ZLinkLocationEventSink;
  readonly leaseTracker?: ZLinkOwnerLeaseTracker;
  readonly now?: () => Date;
  readonly monotonicNowMs?: () => number;
  readonly setTimer?: (callback: () => void, delayMs: number) => unknown;
  readonly clearTimer?: (handle: unknown) => void;
  readonly metrics?: import('../diagnostics').ZLinkRuntimeMetrics;
  readonly meshNames?: readonly string[];
  readonly leaseScopes?: readonly {
    readonly kind: 'mesh' | 'channel';
    readonly name: string;
  }[];
  readonly rewriteAuthorityPayloadForOwner?: (
    payload: Uint8Array,
    owner: ZLinkLocationOwnerToken
  ) => Uint8Array;
}

export interface ZLinkLocationEventSink {
  peerRowUpdated(key: ZLinkLocationKey, peer: ZLinkPeerLocation): void;
  peerRowRemoved(key: ZLinkLocationKey): void;
  desiredSetChanged(change: {
    readonly autoConnectType: ZLinkLocationAutoConnectType;
    readonly meshName: string;
    readonly connectedEndpoints: readonly string[];
    readonly disconnectedEndpoints: readonly string[];
  }): void;
  spotRowUpdated(key: ZLinkSpotLocationKey, spot: ZLinkSpotLocation): void;
  spotRowRemoved(key: ZLinkSpotLocationKey): void;
  spotResolveMiss(key: ZLinkSpotLocationKey): void;
  actorRowUpdated(key: ZLinkActorLocationKey, actor: ZLinkActorLocation): void;
  actorRowRemoved(key: ZLinkActorLocationKey): void;
  actorResolveMiss(key: ZLinkActorLocationKey): void;
  routeRowUpdated(key: ZLinkRouteLocationKey, route: ZLinkRouteLocation): void;
  routeRowRemoved(key: ZLinkRouteLocationKey): void;
  routeResolveMiss(key: ZLinkRouteLocationKey): void;
}

export class ZLinkOwnerCleanupError extends Error {
  constructor(cause: unknown) {
    super(`Owner cleanup failed: ${errorMessage(cause)}`, { cause });
    this.name = 'ZLinkOwnerCleanupError';
  }
}

export class ZLinkOwnerLeaseClaimError extends Error {
  constructor(readonly kind: 'conflict' | 'generationExhausted') {
    super(`Owner lease claim failed with '${kind}'.`);
    this.name = 'ZLinkOwnerLeaseClaimError';
  }
}

interface ZLinkRetainedExecution {
  readonly recover?: ZLinkExecutionRecovery;
  phase: 'retained' | 'recovering' | 'recovered' | Promise<void>;
}

export class ZLinkLocationRuntime implements ZLinkLocationRuntimeQuery {
  readonly ownerId: string;
  private readonly options: Required<ZLinkLocationOptionOverrides>;
  private readonly stores: ZLinkLocationRuntimeStores;
  private readonly events?: ZLinkLocationEventSink;
  private readonly setTimer: (callback: () => void, delayMs: number) => unknown;
  private readonly clearTimer: (handle: unknown) => void;
  private readonly monotonicNowMs: () => number;
  private readonly liveRows: ZLinkLiveRowFilter;
  private readonly leaseTracker: ZLinkOwnerLeaseTracker;
  private readonly ownershipLostHandlers = new Set<(event: ZLinkOwnershipLostEvent) => void>();
  private readonly ownerLeaseRenewedHandlers = new Set<
    (renewal: ZLinkOwnerLeaseRenewed, wasOwnerLeaseUsable: boolean) => void
  >();
  private readonly ownerLeaseRenewalFailedHandlers = new Set<() => void>();
  private heartbeatTimer: unknown;
  private heartbeatRenewal?: Promise<unknown>;
  private nodeRidValue?: RoutingId;
  private started = false;
  private ownerCleanupComplete = true;
  private readonly metrics?: import('../diagnostics').ZLinkRuntimeMetrics;
  private readonly meshNames: readonly string[];
  private readonly leaseScopes: readonly {
    readonly kind: 'mesh' | 'channel';
    readonly name: string;
  }[];
  private readonly rewriteAuthorityPayloadForOwner?: ZLinkLocationRuntimeOptions['rewriteAuthorityPayloadForOwner'];
  private leaseOperationStartedAtMs = 0;
  private ownerToken?: ZLinkLocationOwnerToken;
  private lastOwnerToken?: ZLinkLocationOwnerToken;
  private ownerLeaseDeadlineMs?: number;
  private lastSuccessfulLease?: {
    readonly startedAtMs: number;
    readonly admissionDeadlineMs: number;
    readonly owner: ZLinkLocationOwnerToken;
  };
  private storeOutage?: {
    readonly startedAtMs: number;
    readonly cutoffMs: number;
    readonly owner: ZLinkLocationOwnerToken;
    discarded: boolean;
    readonly retained: Map<WeakRef<object>, ZLinkRetainedExecution>;
    readonly retainedByOwner: WeakMap<object, ZLinkRetainedExecution>;
  };
  private readonly discardedExecutions = new WeakSet<object>();
  private readonly executions = new Map<
    object,
    {
      readonly steadyReady: () => boolean;
      readonly discard: (error: unknown) => void;
      readonly captureRecovery?: (reference: WeakRef<object>) => ZLinkExecutionRecovery | undefined;
    }
  >();
  private ownerDeadlineTimer: unknown;
  private ownerAdmissionClosed = true;

  trackExecution(
    owner: object,
    steadyReady: () => boolean,
    discard: (error: unknown) => void,
    captureRecovery?: (reference: WeakRef<object>) => ZLinkExecutionRecovery | undefined
  ): void {
    if (!this.discardedExecutions.has(owner) && !this.executions.has(owner))
      this.executions.set(owner, { steadyReady, discard, captureRecovery });
  }

  forgetExecution(owner: object): void {
    this.executions.delete(owner);
  }

  discardExecution(owner: object): void {
    if (!this.started) return;
    const execution = this.executions.get(owner);
    this.discardedExecutions.add(owner);
    this.forgetExecution(owner);
    execution?.discard(this.executionFailure());
  }

  discardSteadyExecution(owner: object): boolean {
    if (this.executions.get(owner)?.steadyReady() !== true) return false;
    this.discardExecution(owner);
    return true;
  }

  private retainedExecutions():
    ReadonlyMap<WeakRef<object>, { readonly phase: string | Promise<void> }> | undefined {
    const outage = this.storeOutage;
    if (outage !== undefined && this.monotonicNowMs() >= outage.cutoffMs) this.latchStoreOutage();
    return this.options.storeOutageRetention && outage !== undefined && !outage.discarded
      ? outage.retained
      : undefined;
  }

  private executionFailure() {
    return createInternalFrameworkException(
      this.started
        ? ZLinkFrameworkInternalErrorKind.RequestTargetNotFound
        : ZLinkFrameworkInternalErrorKind.RuntimeShutdown,
      this.started ? 'Object execution authority was discarded.' : 'Location runtime is stopped.'
    );
  }

  executionAllowed(owner: object | undefined): boolean {
    if (!this.started) throw this.executionFailure();
    if (this.storeOutage === undefined) {
      if (owner !== undefined && this.discardedExecutions.has(owner)) throw this.executionFailure();
      return (
        this.ownerToken !== undefined &&
        this.ownerLeaseDeadlineMs !== undefined &&
        !this.ownerAdmissionClosed
      );
    }
    const retained = this.retainedExecutions();
    if (owner !== undefined && this.discardedExecutions.has(owner)) throw this.executionFailure();
    const record = owner === undefined ? undefined : this.storeOutage.retainedByOwner.get(owner);
    if (record !== undefined && record.phase !== 'retained' && record.phase !== 'recovered')
      return false;
    return this.ownerLeaseUsable || (record?.phase === 'retained' && retained !== undefined);
  }

  get boundExecutionUsable(): boolean {
    const retained = this.retainedExecutions();
    return this.ownerLeaseUsable || (retained !== undefined && retained.size > 0);
  }

  [inspectRuntime]() {
    return {
      lastSuccessfulLeaseStartedAtMs: this.lastSuccessfulLease?.startedAtMs,
      storeOutage:
        this.storeOutage === undefined
          ? undefined
          : {
              startedAtMs: this.storeOutage.startedAtMs,
              cutoffMs: this.storeOutage.cutoffMs,
              discarded: this.storeOutage.discarded
            }
    };
  }

  ownerLeaseHealthy = false;
  ownerLeaseRenewedAt?: Date;
  lastError?: string;

  get ownerLeaseUsable(): boolean {
    return (
      this.ownerToken !== undefined &&
      this.ownerLeaseDeadlineMs !== undefined &&
      this.monotonicNowMs() < this.ownerLeaseDeadlineMs
    );
  }

  constructor(runtimeOptions: ZLinkLocationRuntimeOptions) {
    this.stores = runtimeOptions.stores;
    this.options = { ...zlinkRuntimeDefaultLocationOptions, ...runtimeOptions.options };
    this.ownerId = runtimeOptions.ownerId ?? randomUUID().replaceAll('-', '');
    this.events = runtimeOptions.events;
    this.metrics = runtimeOptions.metrics;
    this.meshNames = [...new Set(runtimeOptions.meshNames ?? [])];
    this.leaseScopes =
      runtimeOptions.leaseScopes ?? this.meshNames.map((name) => ({ kind: 'mesh', name }));
    this.rewriteAuthorityPayloadForOwner = runtimeOptions.rewriteAuthorityPayloadForOwner;
    this.monotonicNowMs = runtimeOptions.monotonicNowMs ?? (() => performance.now());
    this.setTimer =
      runtimeOptions.setTimer ?? ((callback, delayMs) => setTimeout(callback, delayMs));
    this.clearTimer =
      runtimeOptions.clearTimer ?? ((handle) => clearTimeout(handle as NodeJS.Timeout));
    this.leaseTracker =
      runtimeOptions.leaseTracker ??
      new ZLinkOwnerLeaseTracker({
        store: this.stores.ownerLeaseStore,
        options: this.options,
        monotonicNowMs: this.monotonicNowMs
      });
    this.liveRows = new ZLinkLiveRowFilter();
  }

  get isStarted(): boolean {
    return this.started;
  }

  get nodeRid(): RoutingId | undefined {
    return this.nodeRidValue;
  }

  get currentOwnerToken(): ZLinkLocationOwnerToken | undefined {
    return this.ownerToken;
  }

  /** Location runtime §5: a descriptor publication ends within the renew timeout. */
  async writeDescriptor<T>(
    action: (signal?: AbortSignal) => Promise<T>,
    signal?: AbortSignal
  ): Promise<T> {
    try {
      return await withTimeout(action, this.options.ownerLeaseRenewTimeoutMs, signal);
    } catch (error) {
      if (error instanceof ZLinkOwnerLeaseReleaseDelayPendingError) throw error;
      if (!isIndeterminateLocationStoreFailure(error, signal)) throw error;
      throw new ZLinkDescriptorPublicationUnconfirmedError(error);
    }
  }

  closeOwnerLeaseAdmission(error: unknown): void {
    this.ownerLeaseDeadlineMs = undefined;
    this.recordFailure(errorMessage(error));
  }

  get ownerLeaseRecoveryRequired(): boolean {
    if (this.storeOutage !== undefined)
      for (const record of this.storeOutage.retained.values())
        if (record.phase !== 'recovered') return true;
    return false;
  }

  async reclaimOwnerAuthorities(
    publication: Promise<void> = Promise.resolve(),
    signal?: AbortSignal,
    onConfirmed?: ZLinkAuthorityConfirmation
  ): Promise<ZLinkPreservedAuthority> {
    const outage = this.storeOutage;
    const owner = this.ownerToken;
    const retainedIncarnations = new Map<string, Map<bigint, Set<bigint>>>();
    if (outage !== undefined)
      for (const record of outage.retained.values()) {
        const recovery = record.recover;
        if (recovery === undefined) continue;
        let generations = retainedIncarnations.get(recovery.key.value);
        if (generations === undefined) {
          generations = new Map();
          retainedIncarnations.set(recovery.key.value, generations);
        }
        let authorityGenerations = generations.get(recovery.incarnation.objectGeneration);
        if (authorityGenerations === undefined) {
          authorityGenerations = new Set();
          generations.set(recovery.incarnation.objectGeneration, authorityGenerations);
        }
        authorityGenerations.add(recovery.incarnation.authorityOwnerGeneration);
      }
    const excluded: ZLinkPreservedAuthority = (key, incarnation) =>
      incarnation !== undefined &&
      retainedIncarnations
        .get(key.value)
        ?.get(incarnation.objectGeneration)
        ?.has(incarnation.authorityOwnerGeneration) === true;
    // Observe rejection immediately, including when every object is already terminal.
    const confirmedPublication = publication.then(
      () => undefined,
      (error) => {
        if (
          error instanceof ZLinkMeshNodeDescriptorConflictException ||
          !isIndeterminateLocationStoreFailure(error, signal)
        ) {
          for (const reference of outage?.retained.keys() ?? []) {
            const execution = reference.deref();
            if (execution !== undefined) this.discardExecution(execution);
          }
        }
        throw error;
      }
    );
    const recoveries =
      outage === undefined || owner === undefined || outage.discarded
        ? []
        : [...outage.retained].map(([reference, retained]) => {
            if (retained.phase instanceof Promise) return retained.phase;
            return this.recoverExecution(
              outage,
              reference,
              retained,
              owner,
              confirmedPublication,
              signal
            );
          });
    await Promise.all([confirmedPublication, ...recoveries]);
    await this.reclaimUnpreservedOwnerAuthorities(excluded, signal, onConfirmed);
    if (
      outage !== undefined &&
      !outage.discarded &&
      [...outage.retained.values()].every((record) => record.phase === 'recovered')
    ) {
      this.storeOutage = undefined;
      this.scheduleOwnerDeadlines();
    }
    return excluded;
  }

  private recoverExecution(
    outage: NonNullable<ZLinkLocationRuntime['storeOutage']>,
    reference: WeakRef<object>,
    retained: ZLinkRetainedExecution,
    owner: ZLinkLocationOwnerToken,
    publication: Promise<void>,
    signal?: AbortSignal
  ): Promise<void> {
    if (
      reference.deref() === undefined ||
      this.discardedExecutions.has(reference.deref()!) ||
      retained.phase === 'recovered'
    )
      return Promise.resolve();
    if (retained.phase instanceof Promise) return retained.phase;
    const operation = (async () => {
      const recovered = await retained.recover?.run(
        owner,
        () =>
          this.storeOutage === outage &&
          !outage.discarded &&
          reference.deref() !== undefined &&
          !this.discardedExecutions.has(reference.deref()!) &&
          this.monotonicNowMs() + this.options.ownerLeaseRenewTimeoutMs <= outage.cutoffMs,
        signal
      );
      if (recovered !== true) {
        const execution = reference.deref();
        if (execution !== undefined) this.discardExecution(execution);
        return;
      }
      await publication;
      if (this.monotonicNowMs() >= outage.cutoffMs) this.latchStoreOutage();
      const execution = reference.deref();
      if (
        !outage.discarded &&
        execution !== undefined &&
        !this.discardedExecutions.has(execution)
      ) {
        if (
          this.ownerToken?.ownerId === owner.ownerId &&
          this.ownerToken.leaseGeneration === owner.leaseGeneration
        )
          retained.phase = 'recovered';
        else this.discardExecution(execution);
      }
    })();
    retained.phase = operation;
    void operation.catch(() => {
      if (retained.phase === operation) retained.phase = 'recovering';
    });
    return operation;
  }

  private recoverPublishedExecutions(
    meshName: string,
    owner: ZLinkLocationOwnerToken,
    publication: Promise<ZLinkLocationWriteResult>,
    signal?: AbortSignal
  ): void {
    const outage = this.storeOutage;
    if (outage === undefined || outage.discarded) return;
    const confirmed = publication.then((result) => {
      if (result.status !== ZLinkLocationWriteStatus.Stored)
        throw new ZLinkMeshNodeDescriptorConflictException(
          `Recovery descriptor '${meshName}' was rejected: ${result.status}.`
        );
    });
    void confirmed.catch((error) => {
      if (!(error instanceof ZLinkMeshNodeDescriptorConflictException)) return;
      for (const [reference, retained] of outage.retained) {
        const execution = reference.deref();
        if (execution !== undefined && retained.recover?.meshName === meshName)
          this.discardExecution(execution);
      }
    });
    for (const [reference, retained] of outage.retained) {
      if (retained.recover?.meshName !== meshName) continue;
      void this.recoverExecution(outage, reference, retained, owner, confirmed, signal).catch(
        (error) => {
          this.lastError = errorMessage(error);
        }
      );
    }
  }

  private async reclaimUnpreservedOwnerAuthorities(
    excluded: ZLinkPreservedAuthority,
    signal?: AbortSignal,
    onConfirmed?: ZLinkAuthorityConfirmation
  ): Promise<void> {
    const owner = this.ownerToken;
    if (owner === undefined) {
      throw new Error('Authority recovery requires a claimed owner token.');
    }
    const entries = await this.readAllAuthorityEntries(signal);
    const failures: unknown[] = [];
    for (const entry of entries) {
      const snapshot = entry.snapshot;
      if (
        excluded(entry.key, snapshot) ||
        snapshot.ownerId !== owner.ownerId ||
        snapshot.ownerLeaseGeneration === owner.leaseGeneration
      ) {
        continue;
      }
      try {
        const result = await this.stores.authorityStore.compareExchangeAuthority(
          entry.key,
          snapshot.storeVersion,
          {
            kind: 'rebindOwnerLease',
            expectedOwner: {
              ownerId: snapshot.ownerId,
              leaseGeneration: snapshot.ownerLeaseGeneration
            },
            targetOwner: owner,
            payload: Buffer.from(
              this.rewriteAuthorityPayloadForOwner?.(snapshot.payload, owner) ?? snapshot.payload
            )
          },
          signal
        );
        if (result.kind === 'stored') {
          onConfirmed?.(entry.key, snapshot, { ...result, kind: 'snapshot' });
          continue;
        }
        if (result.kind === 'conflict') {
          const current = result.current;
          if (
            current.kind === 'snapshot' &&
            current.ownerId === owner.ownerId &&
            current.ownerLeaseGeneration === owner.leaseGeneration
          ) {
            onConfirmed?.(entry.key, snapshot, current);
            continue;
          }
          continue;
        }
        failures.push(
          new Error(`Authority '${entry.key.value}' recovery returned '${result.kind}'.`)
        );
      } catch (error) {
        failures.push(error);
      }
    }
    if (failures.length > 0) {
      throw new AggregateError(failures, 'One or more authority records failed lease recovery.');
    }
  }

  private async readAllAuthorityEntries(signal?: AbortSignal): Promise<ZLinkAuthorityEntry[]> {
    for (;;) {
      const entries: ZLinkAuthorityEntry[] = [];
      let cursor: ZLinkAuthorityScanCursor | undefined;
      let expired = false;
      do {
        const page = await this.stores.authorityStore.listAuthorities(
          '',
          cursor,
          ZLINK_PROVIDER_MAX_PAGE_SIZE,
          signal
        );
        if (page.kind === 'scanExpired') {
          expired = true;
          break;
        }
        entries.push(...page.items);
        cursor = page.nextCursor;
      } while (cursor !== undefined);
      if (!expired) return entries;
    }
  }

  addOwnershipLostHandler(handler: (event: ZLinkOwnershipLostEvent) => void): void {
    this.ownershipLostHandlers.add(handler);
  }

  removeOwnershipLostHandler(handler: (event: ZLinkOwnershipLostEvent) => void): void {
    this.ownershipLostHandlers.delete(handler);
  }

  addOwnerLeaseRenewedHandler(
    handler: (renewal: ZLinkOwnerLeaseRenewed, wasOwnerLeaseUsable: boolean) => void
  ): void {
    this.ownerLeaseRenewedHandlers.add(handler);
  }

  removeOwnerLeaseRenewedHandler(
    handler: (renewal: ZLinkOwnerLeaseRenewed, wasOwnerLeaseUsable: boolean) => void
  ): void {
    this.ownerLeaseRenewedHandlers.delete(handler);
  }

  addOwnerLeaseRenewalFailedHandler(handler: () => void): void {
    this.ownerLeaseRenewalFailedHandlers.add(handler);
  }

  removeOwnerLeaseRenewalFailedHandler(handler: () => void): void {
    this.ownerLeaseRenewalFailedHandlers.delete(handler);
  }

  start(nodeRid: RoutingId): Promise<void>;
  start(nodeRid: RoutingId, signal: AbortSignal): Promise<void>;
  start(nodeRid: RoutingId, signal: AbortSignal, shutdownSignal: () => AbortSignal): Promise<void>;
  async start(
    nodeRid: RoutingId,
    signal?: AbortSignal,
    shutdownSignal?: () => AbortSignal
  ): Promise<void> {
    if (this.started) {
      return;
    }

    this.started = true;
    this.ownerCleanupComplete = false;
    this.nodeRidValue = nodeRid;
    const claim = this.renewOwnerLeaseOnce(signal, shutdownSignal);
    this.heartbeatRenewal = claim;
    try {
      await claim;
      this.scheduleHeartbeat();
    } catch (error) {
      this.started = false;
      this.ownerCleanupComplete = true;
      throw signal?.aborted === true ? signal.reason : error;
    }
  }

  async stop(signal?: AbortSignal): Promise<void> {
    this.started = false;
    if (this.heartbeatTimer !== undefined) {
      this.clearTimer(this.heartbeatTimer);
      this.heartbeatTimer = undefined;
    }
    if (this.ownerDeadlineTimer !== undefined) {
      this.clearTimer(this.ownerDeadlineTimer);
      this.ownerDeadlineTimer = undefined;
    }
    this.leaseOperationStartedAtMs = 0;
    this.ownerLeaseDeadlineMs = undefined;
    await this.cleanupOwner(signal);
  }

  async cleanupOwner(signal?: AbortSignal): Promise<void> {
    const pending = this.heartbeatRenewal;
    try {
      await (pending ?? Promise.resolve());
    } catch (error) {
      if (!isAbortError(error)) throw error;
    } finally {
      if (this.heartbeatRenewal === pending) this.heartbeatRenewal = undefined;
    }
    if (this.ownerCleanupComplete) {
      return;
    }
    try {
      const owner = this.ownerToken ?? this.lastOwnerToken;
      signal?.throwIfAborted();
      if (owner !== undefined) {
        await this.stores.locationStore.removeAllByOwner(owner, signal);
        signal?.throwIfAborted();
        await this.stores.ownerLeaseStore.releaseOwnerLease(owner, signal);
        signal?.throwIfAborted();
      }
      this.ownerToken = undefined;
      this.lastOwnerToken = undefined;
      this.ownerCleanupComplete = true;
    } catch (error) {
      this.recordFailure(errorMessage(error));
      throw new ZLinkOwnerCleanupError(error);
    }
  }

  async renewOwnerLeaseOnce(
    signal?: AbortSignal,
    shutdownSignal?: () => AbortSignal
  ): Promise<boolean> {
    if (!this.started) {
      return false;
    }
    if (this.ownerToken === undefined) {
      return await this.claimFreshOwnerLease(signal, shutdownSignal);
    }

    try {
      const renewingOwner = this.ownerToken;
      const renewStartedAtMs = this.monotonicNowMs();
      this.leaseOperationStartedAtMs = renewStartedAtMs;
      const result = await withTimeout(
        (renewSignal) =>
          this.stores.ownerLeaseStore.renewOwnerLease(
            renewingOwner,
            this.options.ownerLeaseTtlMs,
            renewSignal
          ),
        this.options.ownerLeaseRenewTimeoutMs,
        signal
      ).catch((error: unknown) => {
        this.observeStoreOutage(error, signal);
        throw error;
      });
      const renewCompletedAtMs = this.monotonicNowMs();
      const outage = this.storeOutage;
      if (result.kind !== 'renewed' && outage !== undefined) {
        const current = await withTimeout(
          (readSignal) =>
            this.stores.ownerLeaseStore.readOwnerLease(renewingOwner.ownerId, readSignal),
          this.options.ownerLeaseRenewTimeoutMs,
          signal
        ).catch((error: unknown) => {
          this.observeStoreOutage(error, signal);
          throw error;
        });
        if (this.ownerToken !== renewingOwner || this.storeOutage !== outage) return false;
        const changedOwner =
          current.kind === 'found' &&
          (current.token.ownerId !== renewingOwner.ownerId ||
            current.token.leaseGeneration !== renewingOwner.leaseGeneration);
        if (changedOwner) {
          outage.discarded = true;
          for (const reference of outage.retained.keys()) {
            const execution = reference.deref();
            if (execution !== undefined) this.discardExecution(execution);
          }
        }
        // Admission ends at lifetime - margin; an outage claim needs lifetime + margin
        // and an exact read confirming normal expiry (Location runtime §5.1, §10).
        if (
          changedOwner ||
          this.lastSuccessfulLease === undefined ||
          this.monotonicNowMs() <
            this.lastSuccessfulLease.admissionDeadlineMs +
              2 * this.options.ownerLeaseFencingMarginMs ||
          (current.kind === 'found' &&
            !(current.leaseExpiresAt.getTime() <= current.storeNow.getTime()))
        ) {
          this.ownerLeaseDeadlineMs = undefined;
          this.recordOwnerLeaseRenewFailure();
          this.recordFailure('Owner lease token cannot be renewed.');
          for (const handler of this.ownerLeaseRenewalFailedHandlers) handler();
          return false;
        }
      }
      if (result.kind !== 'renewed') {
        this.lastOwnerToken = this.ownerToken;
        this.ownerToken = undefined;
        this.ownerLeaseHealthy = false;
        this.ownerLeaseDeadlineMs = undefined;
        this.recordFailure('Owner lease token is stale.');
        for (const handler of this.ownerLeaseRenewalFailedHandlers) handler();
        // Claim after the outage eligibility check, or immediately outside an episode.
        return await this.claimFreshOwnerLease(signal, shutdownSignal);
      }
      if (this.ownerToken !== renewingOwner) {
        this.retainedExecutions();
        return false;
      }
      const wasOwnerLeaseUsable = this.ownerLeaseUsable;
      this.ownerLeaseHealthy = true;
      this.ownerLeaseRenewedAt = result.storeNow;
      this.setOwnerLeaseDeadline(
        renewStartedAtMs,
        renewCompletedAtMs,
        result.leaseExpiresAt,
        result.storeNow
      );
      this.recordSuccessfulLease(renewStartedAtMs, renewingOwner, true);
      this.lastError = undefined;
      for (const handler of this.ownerLeaseRenewedHandlers) {
        handler(result, wasOwnerLeaseUsable);
      }
      return true;
    } catch (error) {
      if (signal?.aborted === true) throw error;
      this.recordOwnerLeaseRenewFailure();
      this.recordFailure(errorMessage(error));
      for (const handler of this.ownerLeaseRenewalFailedHandlers) handler();
      return false;
    }
  }

  private async claimFreshOwnerLease(
    signal?: AbortSignal,
    shutdownSignal?: () => AbortSignal
  ): Promise<boolean> {
    const claimStartedAtMs = this.monotonicNowMs();
    this.leaseOperationStartedAtMs = claimStartedAtMs;
    const timeoutStartedAtMs = performance.now();
    let claim: Awaited<ReturnType<ZLinkOwnerLeaseStore['claimOwnerLease']>>;
    try {
      claim = await this.claimOwnerLeaseWithConfirmation(
        timeoutStartedAtMs,
        signal,
        shutdownSignal
      );
    } catch (error) {
      if (signal?.aborted === true) throw error;
      if (error instanceof ZLinkOwnerLeaseClaimError) throw error;
      this.recordOwnerLeaseRenewFailure();
      this.recordFailure(errorMessage(error));
      return false;
    }
    const claimCompletedAtMs = this.monotonicNowMs();
    if (claim.kind !== 'claimed') {
      const error = new ZLinkOwnerLeaseClaimError(claim.kind);
      this.recordFailure(error.message);
      throw error;
    }
    try {
      if (!this.started) {
        this.lastOwnerToken = claim.token;
        return false;
      }
      this.lastOwnerToken = this.ownerToken;
      this.ownerToken = claim.token;
      this.ownerLeaseHealthy = true;
      this.ownerLeaseRenewedAt = claim.storeNow;
      this.setOwnerLeaseDeadline(
        claimStartedAtMs,
        claimCompletedAtMs,
        claim.leaseExpiresAt,
        claim.storeNow
      );
      this.recordSuccessfulLease(claimStartedAtMs, claim.token, false);
      this.lastError = undefined;
      for (const handler of this.ownerLeaseRenewedHandlers) {
        handler(
          {
            kind: 'renewed',
            leaseExpiresAt: claim.leaseExpiresAt,
            storeNow: claim.storeNow
          },
          false
        );
      }
      return true;
    } catch (error) {
      if (signal?.aborted === true) throw error;
      this.recordFailure(errorMessage(error));
      return false;
    }
  }

  private async claimOwnerLeaseWithConfirmation(
    timeoutStartedAtMs: number,
    signal?: AbortSignal,
    shutdownSignal?: () => AbortSignal
  ): Promise<Awaited<ReturnType<ZLinkOwnerLeaseStore['claimOwnerLease']>>> {
    signal?.throwIfAborted();
    let claim: Awaited<ReturnType<ZLinkOwnerLeaseStore['claimOwnerLease']>>;
    let request: ReturnType<ZLinkOwnerLeaseStore['claimOwnerLease']> | undefined;
    try {
      claim = await withTimeout(
        (claimSignal) =>
          (request = this.stores.ownerLeaseStore.claimOwnerLease(
            this.ownerId,
            this.options.ownerLeaseTtlMs,
            claimSignal
          )),
        remainingTimeoutMs(timeoutStartedAtMs, this.options.ownerLeaseRenewTimeoutMs),
        signal
      );
    } catch (error) {
      if (error instanceof OwnerLeaseRenewTimeoutError) throw error;
      if (signal?.aborted === true && request !== undefined) {
        const cleanupSignal = shutdownSignal?.();
        try {
          await withTimeout(
            () => request!,
            cleanupSignal === undefined
              ? remainingTimeoutMs(timeoutStartedAtMs, this.options.ownerLeaseRenewTimeoutMs)
              : undefined,
            cleanupSignal
          );
        } catch (completionError) {
          if (
            cleanupSignal?.aborted === true ||
            completionError instanceof OwnerLeaseRenewTimeoutError
          ) {
            throw new ZLinkOwnerCleanupError(completionError);
          }
          if (!isAbortError(completionError)) {
            this.recordFailure(errorMessage(completionError));
          }
        }
      }
      const confirmed = await this.confirmOwnerLease(
        error,
        timeoutStartedAtMs,
        signal,
        shutdownSignal
      );
      if (confirmed !== undefined) return confirmed;
      throw error;
    }
    if (claim.kind !== 'conflict') return claim;
    const conflict = new ZLinkOwnerLeaseClaimError(claim.kind);
    return (
      (await this.confirmOwnerLease(conflict, timeoutStartedAtMs, signal, shutdownSignal)) ?? claim
    );
  }

  private async confirmOwnerLease(
    originalError: unknown,
    timeoutStartedAtMs: number,
    signal?: AbortSignal,
    shutdownSignal?: () => AbortSignal
  ): Promise<
    | Extract<Awaited<ReturnType<ZLinkOwnerLeaseStore['claimOwnerLease']>>, { kind: 'claimed' }>
    | undefined
  > {
    const cancelled = signal?.aborted === true;
    const cleanupSignal = cancelled ? shutdownSignal?.() : undefined;
    const confirmationTimeoutMs =
      cleanupSignal === undefined
        ? remainingTimeoutMs(timeoutStartedAtMs, this.options.ownerLeaseRenewTimeoutMs)
        : undefined;
    if (confirmationTimeoutMs !== undefined && confirmationTimeoutMs <= 0) throw originalError;
    let confirmed: Awaited<ReturnType<ZLinkOwnerLeaseStore['readOwnerLease']>>;
    try {
      confirmed = await withTimeout(
        (readSignal) => this.stores.ownerLeaseStore.readOwnerLease(this.ownerId, readSignal),
        confirmationTimeoutMs,
        cleanupSignal
      );
    } catch (confirmationError) {
      if (!cancelled) this.observeStoreOutage(confirmationError, cleanupSignal);
      this.recordFailure(errorMessage(confirmationError));
      if (cancelled) throw new ZLinkOwnerCleanupError(confirmationError);
      throw originalError instanceof ZLinkOwnerLeaseClaimError ? originalError : confirmationError;
    }
    if (confirmed.kind !== 'found' || confirmed.token.ownerId !== this.ownerId) {
      throw originalError;
    }
    if (signal?.aborted === true) {
      try {
        await withTimeout(
          (releaseSignal) =>
            this.stores.ownerLeaseStore.releaseOwnerLease(confirmed.token, releaseSignal),
          cleanupSignal === undefined
            ? remainingTimeoutMs(timeoutStartedAtMs, this.options.ownerLeaseRenewTimeoutMs)
            : undefined,
          cleanupSignal
        );
      } catch (releaseError) {
        this.recordFailure(errorMessage(releaseError));
        throw new ZLinkOwnerCleanupError(releaseError);
      }
      throw originalError;
    }
    return {
      kind: 'claimed',
      token: confirmed.token,
      leaseExpiresAt: confirmed.leaseExpiresAt,
      storeNow: confirmed.storeNow
    };
  }

  private observeStoreOutage(error: unknown, signal?: AbortSignal): void {
    if (!this.started || !isIndeterminateLocationStoreFailure(error, signal)) return;
    const lease = this.lastSuccessfulLease;
    if (this.storeOutage !== undefined || lease === undefined) return;
    const cutoffMs = this.options.storeOutageRetention
      ? lease.startedAtMs +
        this.options.ownerLeaseTtlMs +
        2 * this.options.ownerLeaseFencingMarginMs
      : lease.admissionDeadlineMs;
    const startedAtMs = this.monotonicNowMs();
    const retained = new Map<WeakRef<object>, ZLinkRetainedExecution>();
    const retainedByOwner = new WeakMap<object, ZLinkRetainedExecution>();
    for (const [owner, execution] of [...this.executions].filter(([, entry]) =>
      entry.steadyReady()
    )) {
      const reference = new WeakRef(owner);
      const record: ZLinkRetainedExecution = {
        recover: execution.captureRecovery?.(reference),
        phase: 'retained'
      };
      retained.set(reference, record);
      retainedByOwner.set(owner, record);
    }
    this.storeOutage = {
      startedAtMs,
      cutoffMs,
      owner: lease.owner,
      discarded: false,
      retained,
      retainedByOwner
    };
    this.scheduleOwnerDeadlines();
  }

  private scheduleOwnerDeadlines(): void {
    if (this.ownerDeadlineTimer !== undefined) this.clearTimer(this.ownerDeadlineTimer);
    this.ownerDeadlineTimer = undefined;
    if (!this.started) return;
    const now = this.monotonicNowMs();
    if (this.ownerLeaseDeadlineMs === undefined || now >= this.ownerLeaseDeadlineMs)
      this.ownerAdmissionClosed = true;
    this.latchStoreOutage();
    let next = this.ownerAdmissionClosed ? undefined : this.ownerLeaseDeadlineMs;
    const outage = this.storeOutage;
    if (outage !== undefined && !outage.discarded)
      next = next === undefined ? outage.cutoffMs : Math.min(next, outage.cutoffMs);
    if (next === undefined) return;
    // Admission expiry and retention expiry share the existing one-shot reservation.
    this.ownerDeadlineTimer = this.setTimer(
      () => {
        this.ownerDeadlineTimer = undefined;
        this.scheduleOwnerDeadlines();
      },
      Math.max(0, next - now)
    );
  }

  private latchStoreOutage(): void {
    const outage = this.storeOutage;
    if (outage === undefined || outage.discarded) return;
    if (this.monotonicNowMs() >= outage.cutoffMs) {
      outage.discarded = true;
      for (const [reference, record] of outage.retained) {
        const owner = reference.deref();
        if (owner !== undefined && record.phase !== 'recovered') this.discardSteadyExecution(owner);
      }
      return;
    }
  }

  private recordSuccessfulLease(
    startedAtMs: number,
    owner: ZLinkLocationOwnerToken,
    renewal: boolean
  ): void {
    if (
      this.lastSuccessfulLease === undefined ||
      startedAtMs >= this.lastSuccessfulLease.startedAtMs
    ) {
      this.lastSuccessfulLease = {
        startedAtMs,
        admissionDeadlineMs: this.ownerLeaseDeadlineMs!,
        owner
      };
    }
    const outage = this.storeOutage;
    if (outage !== undefined) {
      for (const retained of outage.retained.values())
        if (retained.phase === 'retained') retained.phase = 'recovering';
      // A completion applied at T cannot undo expiry even if its callback runs first.
      if (this.monotonicNowMs() >= outage.cutoffMs) this.latchStoreOutage();
      if (
        outage.discarded ||
        !renewal ||
        owner.ownerId !== outage.owner.ownerId ||
        owner.leaseGeneration !== outage.owner.leaseGeneration
      )
        return;
      if (outage.retained.size > 0) return;
      this.storeOutage = undefined;
      this.scheduleOwnerDeadlines();
    }
  }

  private setOwnerLeaseDeadline(
    requestStartedAtMs: number,
    requestCompletedAtMs: number,
    leaseExpiresAt: Date,
    storeNow: Date
  ): void {
    const roundTripMs = Math.max(0, requestCompletedAtMs - requestStartedAtMs);
    const remainingTtlMs = Math.max(0, leaseExpiresAt.getTime() - storeNow.getTime());
    // The Store's remaining TTL is measured at an unknown point during the
    // request. Subtract the full observed round trip and the fencing margin
    // so local eligibility cannot outlive the conservative pre-request bound.
    this.ownerLeaseDeadlineMs =
      requestCompletedAtMs +
      Math.max(0, remainingTtlMs - roundTripMs - this.options.ownerLeaseFencingMarginMs);
    this.ownerAdmissionClosed = requestCompletedAtMs >= this.ownerLeaseDeadlineMs;
    this.scheduleOwnerDeadlines();
  }

  async publishDraining(signal?: AbortSignal): Promise<boolean> {
    try {
      const peers = await this.stores.peerStore.listPeers({}, signal);
      for (const peer of peers) {
        if (peer.ownerId !== this.ownerId || peer.draining) continue;
        const result = await this.stores.peerStore.updatePeer(
          { ...peer, draining: true },
          ZLinkLocationWriteIntent.Renew,
          signal
        );
        if (result.status !== ZLinkLocationWriteStatus.Stored) return false;
      }
      return true;
    } catch (error) {
      this.recordFailure(errorMessage(error));
      return false;
    }
  }

  async writeMeshNode(
    descriptor: Omit<ZLinkMeshNodeDescriptor, 'ownerId' | 'leaseGeneration' | 'updatedAt'>,
    intent: ZLinkMeshNodeWriteIntent,
    signal?: AbortSignal
  ): Promise<ZLinkLocationWriteResult> {
    const owner = this.ownerToken;
    if (owner === undefined) {
      throw new Error('MeshNode descriptor write requires a claimed owner token.');
    }
    const stamped: ZLinkMeshNodeDescriptor = {
      ...descriptor,
      ownerId: owner.ownerId,
      leaseGeneration: owner.leaseGeneration,
      // The canonical opaque descriptor carries the time of this write.
      // It is part of the stored envelope, so it must be stamped before
      // handing the descriptor to the provider rather than only on the
      // in-process result after the write completes.
      updatedAt: new Date()
    };
    const submit = (writeIntent: ZLinkMeshNodeWriteIntent) => {
      const request = this.guardWrite(() =>
        this.writeDescriptor(
          (writeSignal) =>
            this.stores.locationStore.updateMeshNode(stamped, writeIntent, writeSignal),
          signal
        )
      );
      const outage = this.storeOutage;
      if (
        outage !== undefined &&
        (owner.leaseGeneration === outage.owner.leaseGeneration ||
          writeIntent === ZLinkInternalMeshNodeWriteIntent.RecoveryClaim ||
          writeIntent === ZLinkInternalMeshNodeWriteIntent.FixedRidNewClaim)
      )
        this.recoverPublishedExecutions(stamped.meshName, owner, request, signal);
      return request;
    };
    let result = await submit(intent);
    if (
      (intent === ZLinkLocationWriteIntent.Renew ||
        intent === ZLinkInternalMeshNodeWriteIntent.FixedRidRenew) &&
      (result.status === ZLinkLocationWriteStatus.RejectedConflict ||
        result.status === ZLinkLocationWriteStatus.IgnoredStale)
    ) {
      // Recovery preserves the existing descriptor publication contract.
      result = await submit(
        intent === ZLinkInternalMeshNodeWriteIntent.FixedRidRenew
          ? ZLinkInternalMeshNodeWriteIntent.FixedRidNewClaim
          : ZLinkInternalMeshNodeWriteIntent.RecoveryClaim
      );
    }
    if (result.status === ZLinkLocationWriteStatus.Stored)
      this.recoverPublishedExecutions(stamped.meshName, owner, Promise.resolve(result), signal);
    this.notifyIfStale(
      result,
      ZLinkLocationKind.Peer,
      `${descriptor.meshName}\0${String(descriptor.rid)}`
    );
    return result;
  }

  async writePeer(
    peer: ZLinkPeerLocation,
    intent: ZLinkLocationWriteIntent,
    signal?: AbortSignal
  ): Promise<ZLinkLocationWriteResult> {
    zlinkLocationAutoConnectTypeName(peer.autoConnectType);
    zlinkLocationRoleName(peer.role);
    // Normalize on write, not just on listPeerLocations()'s read: the row
    // key is derived from this same endpoint, and a caller round-tripping a
    // row it just read (e.g. to remove it) must compute the same key the
    // row was stored under.
    const normalizedEndpoint = normalizeEndpoint(peer.endpoint);
    const stamped = { ...peer, endpoint: normalizedEndpoint, ownerId: this.ownerId };
    const result = await this.guardWrite(() =>
      this.writeDescriptor(
        (writeSignal) => this.stores.peerStore.updatePeer(stamped, intent, writeSignal),
        signal
      )
    );
    const key: ZLinkPeerLocationKey = {
      autoConnectType: peer.autoConnectType,
      meshName: peer.meshName,
      role: peer.role,
      nodeRid: peer.nodeRid,
      endpoint: normalizedEndpoint
    };
    const rowKey = ZLinkLocationKeyCodec.encodePeerKey(key);
    if (result.status === ZLinkLocationWriteStatus.Stored) {
      this.events?.peerRowUpdated(
        { kind: ZLinkLocationKind.Peer, key },
        { ...stamped, generation: result.generation, updatedAt: result.updatedAt }
      );
    }
    this.notifyIfStale(result, ZLinkLocationKind.Peer, rowKey);
    return result;
  }

  async writeSpot(
    spot: ZLinkSpotLocation,
    intent: ZLinkLocationWriteIntent,
    signal?: AbortSignal
  ): Promise<ZLinkLocationWriteResult> {
    const owner = this.ownerToken;
    if (owner === undefined) throw new Error('Spot location write requires a claimed owner token.');
    const stamped = { ...spot, ownerId: owner.ownerId, leaseGeneration: owner.leaseGeneration };
    const result = await this.guardWrite(() =>
      this.stores.spotStore.updateSpot(stamped, intent, signal)
    );
    const key = { meshName: spot.meshName, spotId: spot.spotId };
    if (result.status === ZLinkLocationWriteStatus.Stored) {
      this.events?.spotRowUpdated(key, { ...stamped, updatedAt: result.updatedAt });
    }
    this.notifyIfStale(result, ZLinkLocationKind.Spot, ZLinkLocationKeyCodec.encodeSpotKey(key));
    return result;
  }

  async writeActor(
    actor: ZLinkActorLocation,
    intent: ZLinkLocationWriteIntent,
    signal?: AbortSignal
  ): Promise<ZLinkLocationWriteResult> {
    const owner = this.ownerToken;
    if (owner === undefined)
      throw new Error('Actor location write requires a claimed owner token.');
    const stamped = {
      ...actor,
      actorType: actor.actorType,
      ownerId: owner.ownerId,
      leaseGeneration: owner.leaseGeneration
    };
    const result = await this.guardWrite(() =>
      this.stores.actorStore.updateActor(stamped, intent, signal)
    );
    const key = { meshName: stamped.meshName, actorId: stamped.actorId };
    if (result.status === ZLinkLocationWriteStatus.Stored) {
      this.events?.actorRowUpdated(key, { ...stamped, updatedAt: result.updatedAt });
    }
    this.notifyIfStale(result, ZLinkLocationKind.Actor, ZLinkLocationKeyCodec.encodeActorKey(key));
    return result;
  }

  async writeRoute(
    route: ZLinkRouteLocation,
    intent: ZLinkLocationWriteIntent,
    signal?: AbortSignal
  ): Promise<ZLinkLocationWriteResult> {
    const stamped = { ...route, ownerId: this.ownerId };
    const result = await this.guardWrite(() =>
      this.stores.routeStore.updateRoute(stamped, intent, signal)
    );
    const key = { routeKind: route.routeKind, routeKey: route.routeKey };
    if (result.status === ZLinkLocationWriteStatus.Stored) {
      this.events?.routeRowUpdated(key, {
        ...stamped,
        generation: result.generation,
        updatedAt: result.updatedAt
      });
    }
    this.notifyIfStale(result, ZLinkLocationKind.Route, ZLinkLocationKeyCodec.encodeRouteKey(key));
    return result;
  }

  async removePeer(
    key: ZLinkPeerLocationKey,
    generation: bigint,
    signal?: AbortSignal
  ): Promise<ZLinkLocationWriteResult> {
    const result = await this.guardWrite(() =>
      this.stores.peerStore.removePeer(
        key,
        { ownerId: this.ownerId, leaseGeneration: generation },
        signal
      )
    );
    if (result.status === ZLinkLocationWriteStatus.Stored) {
      this.events?.peerRowRemoved({ kind: ZLinkLocationKind.Peer, key });
    }
    this.notifyIfStale(result, ZLinkLocationKind.Peer, ZLinkLocationKeyCodec.encodePeerKey(key));
    return result;
  }

  async removeSpot(
    key: ZLinkSpotLocationKey,
    generation: bigint,
    signal?: AbortSignal
  ): Promise<ZLinkLocationWriteStatus> {
    const status = await this.guardStatusWrite(() =>
      this.stores.spotStore.removeSpot(
        key,
        { ownerId: this.ownerId, leaseGeneration: generation },
        signal
      )
    );
    if (status === ZLinkLocationWriteStatus.Stored) {
      this.events?.spotRowRemoved(key);
    }
    this.notifyIfStaleStatus(
      status,
      ZLinkLocationKind.Spot,
      ZLinkLocationKeyCodec.encodeSpotKey(key)
    );
    return status;
  }

  async removeActor(
    key: ZLinkActorLocationKey,
    generation: bigint,
    signal?: AbortSignal
  ): Promise<ZLinkLocationWriteStatus> {
    const status = await this.guardStatusWrite(() =>
      this.stores.actorStore.removeActor(
        key,
        { ownerId: this.ownerId, leaseGeneration: generation },
        signal
      )
    );
    if (status === ZLinkLocationWriteStatus.Stored) {
      this.events?.actorRowRemoved(key);
    }
    this.notifyIfStaleStatus(
      status,
      ZLinkLocationKind.Actor,
      ZLinkLocationKeyCodec.encodeActorKey(key)
    );
    return status;
  }

  async removeRoute(
    key: ZLinkRouteLocationKey,
    generation: bigint,
    signal?: AbortSignal
  ): Promise<ZLinkLocationWriteResult> {
    const result = await this.guardWrite(() =>
      this.stores.routeStore.removeRoute(
        key,
        { ownerId: this.ownerId, leaseGeneration: generation },
        signal
      )
    );
    if (result.status === ZLinkLocationWriteStatus.Stored) {
      this.events?.routeRowRemoved(key);
    }
    this.notifyIfStale(result, ZLinkLocationKind.Route, ZLinkLocationKeyCodec.encodeRouteKey(key));
    return result;
  }

  async getStatus(): Promise<ZLinkLocationRuntimeStatus> {
    return {
      storeHealthy: this.lastError === undefined,
      watchEnabled: false,
      pollingIntervalMs: this.options.pollingIntervalMs,
      lastRefreshAt: this.ownerLeaseRenewedAt,
      lastError: this.lastError,
      ownerLeaseHealthy: this.ownerLeaseUsable,
      ownerLeaseRenewedAt: this.ownerLeaseRenewedAt
    };
  }

  reportDiscoveryFailure(error: unknown): void {
    this.recordFailure(errorMessage(error));
  }

  async listPeerLocations(
    filter: ZLinkPeerLocationFilter,
    signal?: AbortSignal
  ): Promise<readonly ZLinkPeerLocation[]> {
    const rows = await this.stores.peerStore.listPeers(filter, signal);
    const live = await this.liveRows.filter(
      rows,
      (row, signal) => this.leaseTracker.isOwnerLive(row.ownerId, signal),
      signal
    );
    // Location Store rows may have been written before this process adopted
    // the endpoint-notation policy, or by another language/version's writer.
    // Normalize on read so every comparison downstream sees the canonical
    // form regardless of what the store actually holds.
    return live.map((row) => ({ ...row, endpoint: normalizeEndpoint(row.endpoint) }));
  }

  async listLiveMeshNodes(
    meshName: string,
    signal?: AbortSignal
  ): Promise<readonly ZLinkMeshNodeDescriptor[]> {
    const rows = await listAllMeshNodeDescriptors(this.stores.locationStore, meshName, signal);
    const live = await this.liveRows.filter(
      rows,
      (row, signal) => this.leaseTracker.isOwnerTokenLive(row, signal),
      signal
    );
    return live.map((row) => ({ ...row, endpoint: normalizeEndpoint(row.endpoint) }));
  }

  /** Remaining exact owner lease from the same tracker that filters live descriptors. */
  remainingOwnerTokenLeaseMs(
    owner: ZLinkLocationOwnerToken,
    signal?: AbortSignal
  ): Promise<number> {
    return this.leaseTracker.remainingOwnerTokenLeaseMs(owner, signal);
  }

  async listMeshNodeDescriptors(
    meshName: string,
    page?: ZLinkPageRequest,
    signal?: AbortSignal
  ): Promise<ZLinkLocationPage<ZLinkMeshNodeDescriptor>> {
    const rows = await this.stores.locationStore.listMeshNodes(
      meshName,
      this.pageRequest(page),
      signal
    );
    const live = await this.liveRows.filter(
      rows.items,
      (row, signal) => this.leaseTracker.isOwnerTokenLive(row, signal),
      signal
    );
    return {
      items: live.map((row) => ({ ...row, endpoint: normalizeEndpoint(row.endpoint) })),
      continuationToken: rows.continuationToken
    };
  }

  async listSpotLocations(
    filter: ZLinkSpotLocationFilter,
    page?: ZLinkPageRequest,
    signal?: AbortSignal
  ): Promise<ZLinkLocationPage<ZLinkSpotLocation>> {
    const queryStore = this.stores.spotStore as ZLinkSpotLocationStore &
      Partial<ZLinkSpotLocationQueryStore>;
    if (queryStore.listSpots === undefined) {
      throw new Error(
        'The configured Spot location store does not provide operational list queries.'
      );
    }
    const rows = await queryStore.listSpots(filter, this.pageRequest(page), signal);
    return {
      items: await this.liveRows.filter(
        rows.items,
        (row, signal) => this.leaseTracker.isOwnerTokenLive(row, signal),
        signal
      ),
      continuationToken: rows.continuationToken
    };
  }

  async listActorLocations(
    filter: ZLinkActorLocationFilter,
    page?: ZLinkPageRequest,
    signal?: AbortSignal
  ): Promise<ZLinkLocationPage<ZLinkActorLocation>> {
    const queryStore = this.stores.actorStore as ZLinkActorLocationStore &
      Partial<ZLinkActorLocationQueryStore>;
    if (queryStore.listActors === undefined) {
      throw new Error(
        'The configured Actor location store does not provide operational list queries.'
      );
    }
    const rows = await queryStore.listActors(filter, this.pageRequest(page), signal);
    return {
      items: await this.liveRows.filter(
        rows.items,
        (row, signal) => this.leaseTracker.isOwnerTokenLive(row, signal),
        signal
      ),
      continuationToken: rows.continuationToken
    };
  }

  async listObjectLocations(
    filter: ZLinkLocationObjectFilter,
    page: ZLinkPageRequest = {},
    signal?: AbortSignal
  ): Promise<ZLinkLocationPage<ZLinkLocationObjectEntry>> {
    const normalizedPage = this.pageRequest(page);
    const pageSize = normalizedPage.pageSize;
    const requestedCursor = normalizedPage.continuationToken;
    let cursor =
      requestedCursor === undefined ? undefined : PublicAuthorityScanCursor.from(requestedCursor);
    const items: ZLinkLocationObjectEntry[] = [];
    for (;;) {
      // Keep the Store cursor at the public page boundary. Scanning 1000 rows
      // and returning only a smaller page would discard the unreturned rows.
      const result = await this.stores.authorityStore.listAuthorities('', cursor, pageSize, signal);
      if (result.kind === 'scanExpired') {
        throw new Error('The authority query snapshot expired.');
      }
      for (const entry of result.items) {
        const identity = decodeAuthorityKey(entry.key);
        const allocation = entry.snapshot.allocation;
        if (
          allocation.objectKind !== filter.objectKind ||
          (filter.stableType !== undefined && allocation.stableType !== filter.stableType) ||
          (filter.meshName !== undefined && allocation.descriptor.meshName !== filter.meshName)
        )
          continue;
        const live =
          (await this.liveRows.resolve(
            entry,
            (candidate, signal) =>
              this.leaseTracker.isOwnerTokenLive(
                {
                  ownerId: candidate.snapshot.ownerId,
                  leaseGeneration: candidate.snapshot.ownerLeaseGeneration
                },
                signal
              ),
            signal
          )) !== undefined;
        items.push(this.objectLocationEntry(identity.globalId, entry.snapshot, live));
        if (items.length >= pageSize) break;
      }
      const nextCursor = result.nextCursor;
      if (items.length >= pageSize || nextCursor === undefined) {
        return {
          items,
          continuationToken: nextCursor?.encoded
        };
      }
      cursor = nextCursor;
    }
  }

  async findActorLocation(
    actorId: import('../../contracts').ActorId,
    signal?: AbortSignal
  ): Promise<ZLinkLocationObjectEntry | undefined> {
    return await this.findObjectLocation('actor', String(actorId), signal);
  }

  async findSpotLocation(
    spotId: import('../../contracts').SpotId,
    signal?: AbortSignal
  ): Promise<ZLinkLocationObjectEntry | undefined> {
    const found = await this.stores.authorityStore.readAuthority(
      encodeAuthorityKey('user_spot', String(spotId)),
      signal
    );
    if (found.kind === 'missing' || found.allocation.objectKind === 'actor') return undefined;
    const live =
      (await this.liveRows.resolve(
        found,
        (snapshot, signal) =>
          this.leaseTracker.isOwnerTokenLive(
            { ownerId: snapshot.ownerId, leaseGeneration: snapshot.ownerLeaseGeneration },
            signal
          ),
        signal
      )) !== undefined;
    return this.objectLocationEntry(String(spotId), found, live);
  }

  private async findObjectLocation(
    kind: 'actor' | 'user_spot',
    globalId: string,
    signal?: AbortSignal
  ): Promise<ZLinkLocationObjectEntry | undefined> {
    const found = await this.stores.authorityStore.readAuthority(
      encodeAuthorityKey(kind, globalId),
      signal
    );
    if (found.kind === 'missing' || found.allocation.objectKind !== kind) return undefined;
    const live =
      (await this.liveRows.resolve(
        found,
        (snapshot, signal) =>
          this.leaseTracker.isOwnerTokenLive(
            { ownerId: snapshot.ownerId, leaseGeneration: snapshot.ownerLeaseGeneration },
            signal
          ),
        signal
      )) !== undefined;
    return this.objectLocationEntry(globalId, found, live);
  }

  private objectLocationEntry(
    globalId: string,
    snapshot: ZLinkAuthoritySnapshot,
    ownerLive: boolean
  ): ZLinkLocationObjectEntry {
    const allocation = snapshot.allocation;
    return {
      globalId,
      objectGeneration: snapshot.objectGeneration,
      meshName: allocation.descriptor.meshName,
      nodeRid: allocation.descriptor.rid,
      state: allocation.state === 'reserved' ? 'creating' : ownerLive ? 'ready' : 'unavailable',
      stableType: allocation.stableType
    };
  }

  async listRouteLocations(
    filter: ZLinkRouteLocationFilter,
    page?: ZLinkPageRequest,
    signal?: AbortSignal
  ): Promise<ZLinkLocationPage<ZLinkRouteLocation>> {
    const rows = await this.stores.routeStore.listRoutes(filter, this.pageRequest(page), signal);
    return {
      items: await this.liveRows.filter(
        rows.items,
        (row, signal) => this.leaseTracker.isOwnerLive(row.ownerId, signal),
        signal
      ),
      continuationToken: rows.continuationToken
    };
  }

  async listTopology(
    filter: ZLinkLocationTopologyFilter,
    page?: ZLinkPageRequest,
    signal?: AbortSignal
  ): Promise<ZLinkLocationPage<ZLinkLocationTopologyEntry>> {
    const entries: ZLinkLocationTopologyEntry[] = [];
    for (const meshName of this.meshNamesOf(filter.meshName)) {
      for (const descriptor of await listAllMeshNodeDescriptors(
        this.stores.locationStore,
        meshName,
        signal,
        this.options.listPageSize
      )) {
        const live = await this.leaseTracker.isOwnerTokenLive(descriptor, signal);
        const entry: ZLinkLocationTopologyEntry = {
          meshName: descriptor.meshName,
          nodeRid: descriptor.rid,
          endpoint: descriptor.endpoint,
          //  Spec 13 §365는 relocate가 unit seal을 마치면 source가 draining으로
          //  넘어간다고 정하고, §409는 draining node가 새 placement target이 되지
          //  않는다고 정한다. Host lifecycle의 `Draining`만 보면 relocate로 빠지는
          //  node가 계속 accepting으로 보인다.
          draining:
            descriptor.state === ZLinkFrameworkRuntimeState.Draining ||
            descriptor.state === ZLinkFrameworkRuntimeState.Relocating ||
            descriptor.state === ZLinkFrameworkRuntimeState.Relocated,
          state: live ? ZLinkLocationTopologyState.Ready : ZLinkLocationTopologyState.Lost,
          updatedAt: descriptor.updatedAt
        };
        if (
          (filter.nodeRid === undefined || entry.nodeRid === filter.nodeRid) &&
          (filter.state === undefined || entry.state === filter.state)
        ) {
          entries.push(entry);
        }
      }
    }
    return this.pageInMemory(entries, page);
  }

  private pageRequest(
    page: ZLinkPageRequest | undefined
  ): ZLinkPageRequest & { readonly pageSize: number } {
    return page?.pageSize === undefined
      ? { ...page, pageSize: this.options.listPageSize }
      : (page as ZLinkPageRequest & { readonly pageSize: number });
  }

  async listServiceSummaries(
    filter: ZLinkLocationServiceSummaryFilter,
    page?: ZLinkPageRequest,
    signal?: AbortSignal
  ): Promise<ZLinkLocationPage<ZLinkLocationServiceSummary>> {
    const summaries: ZLinkLocationServiceSummary[] = [];
    for (const meshName of this.meshNamesOf(filter.meshName)) {
      const descriptors = await listAllMeshNodeDescriptors(
        this.stores.locationStore,
        meshName,
        signal,
        this.options.listPageSize
      );
      if (descriptors.length === 0) continue;
      const live = await this.liveRows.filter(
        descriptors,
        (row, signal) => this.leaseTracker.isOwnerTokenLive(row, signal),
        signal
      );
      summaries.push({
        meshName,
        totalCount: descriptors.length,
        readyCount: live.length,
        errorCount: 0,
        stoppedCount: descriptors.length - live.length,
        lastUpdatedAt: descriptors.reduce(
          (latest, row) => (row.updatedAt > latest ? row.updatedAt : latest),
          descriptors[0].updatedAt
        )
      });
    }
    return this.pageInMemory(summaries, page);
  }

  private meshNamesOf(meshName: string | undefined): readonly string[] {
    return meshName === undefined ? this.meshNames : [meshName];
  }

  private pageInMemory<T>(
    entries: readonly T[],
    page: ZLinkPageRequest | undefined
  ): ZLinkLocationPage<T> {
    const normalized = this.pageRequest(page);
    const parsedOffset = Number.parseInt(normalized.continuationToken ?? '0', 10);
    const offset = Number.isFinite(parsedOffset) && parsedOffset >= 0 ? parsedOffset : 0;
    const pageSize = normalized.pageSize;
    const items = entries.slice(offset, offset + pageSize);
    const nextOffset = offset + items.length;
    return {
      items,
      continuationToken: nextOffset < entries.length ? String(nextOffset) : undefined
    };
  }

  private scheduleHeartbeat(): void {
    if (!this.started) {
      return;
    }
    const scheduledAt = this.leaseOperationStartedAtMs + this.options.ownerLeaseRenewIntervalMs;
    const delayMs = Math.max(0, scheduledAt - this.monotonicNowMs());
    this.heartbeatTimer = this.setTimer(() => {
      this.heartbeatTimer = undefined;
      const lateness = Math.max(0, this.monotonicNowMs() - scheduledAt) / MILLISECONDS_PER_SECOND;
      for (const scope of this.leaseScopes) {
        this.metrics?.recordOwnerLeaseRenewLateness(lateness, scope.kind, scope.name);
      }
      if (
        this.ownerLeaseDeadlineMs !== undefined &&
        this.monotonicNowMs() >= this.ownerLeaseDeadlineMs
      ) {
        this.ownerLeaseHealthy = false;
        this.recordFailure('Owner lease expired before renewal completed.');
        for (const handler of this.ownerLeaseRenewalFailedHandlers) handler();
      }
      const renewal = this.renewOwnerLeaseOnce()
        .then(
          () => undefined,
          () => undefined
        )
        .finally(() => {
          if (this.heartbeatRenewal === renewal) {
            this.heartbeatRenewal = undefined;
          }
          this.scheduleHeartbeat();
        });
      this.heartbeatRenewal = renewal;
      void renewal;
    }, delayMs);
  }

  private async guardWrite(
    write: () => Promise<ZLinkLocationWriteResult>
  ): Promise<ZLinkLocationWriteResult> {
    try {
      return await write();
    } catch (error) {
      this.recordFailure(errorMessage(error));
      throw error;
    }
  }

  private async guardStatusWrite(
    write: () => Promise<ZLinkLocationWriteStatus>
  ): Promise<ZLinkLocationWriteStatus> {
    try {
      return await write();
    } catch (error) {
      this.recordFailure(errorMessage(error));
      throw error;
    }
  }

  notifyIfStale(result: ZLinkLocationWriteResult, kind: ZLinkLocationKind, key: string): void {
    if (result.status !== ZLinkLocationWriteStatus.IgnoredStale) {
      return;
    }
    for (const handler of this.ownershipLostHandlers) {
      handler({ kind, key });
    }
  }

  private notifyIfStaleStatus(
    status: ZLinkLocationWriteStatus,
    kind: ZLinkLocationKind,
    key: string
  ): void {
    if (status !== ZLinkLocationWriteStatus.IgnoredStale) {
      return;
    }
    for (const handler of this.ownershipLostHandlers) {
      handler({ kind, key });
    }
  }

  private recordFailure(message: string): void {
    this.ownerLeaseHealthy = false;
    this.lastError = message;
  }

  private recordOwnerLeaseRenewFailure(): void {
    for (const scope of this.leaseScopes) {
      this.metrics?.recordOwnerLeaseRenewFailure(scope.kind, scope.name);
    }
  }
}

async function withTimeout<T>(
  action: (signal?: AbortSignal) => Promise<T>,
  timeoutMs: number | undefined,
  signal?: AbortSignal
): Promise<T> {
  const timeoutController = timeoutMs === undefined ? undefined : new AbortController();
  const operationSignal =
    signal === undefined
      ? timeoutController?.signal
      : timeoutController === undefined
        ? signal
        : AbortSignal.any([signal, timeoutController.signal]);
  let timeout: ReturnType<typeof setTimeout> | undefined;
  let abort: (() => void) | undefined;
  const deadline =
    timeoutMs === undefined
      ? undefined
      : new Promise<never>((_resolve, reject) => {
          timeout = setTimeout(() => {
            const error = new OwnerLeaseRenewTimeoutError(timeoutMs);
            timeoutController!.abort(error);
            reject(error);
          }, timeoutMs);
          timeout.unref();
        });
  const cancellation =
    signal === undefined
      ? undefined
      : new Promise<never>((_resolve, reject) => {
          abort = () => reject(signal.reason);
          signal.addEventListener('abort', abort, { once: true });
        });
  try {
    signal?.throwIfAborted();
    const operation = action(operationSignal);
    if (deadline === undefined) {
      return cancellation === undefined
        ? await operation
        : await Promise.race([operation, cancellation]);
    }
    return await Promise.race(
      cancellation === undefined ? [operation, deadline] : [operation, deadline, cancellation]
    );
  } finally {
    if (timeout !== undefined) clearTimeout(timeout);
    if (abort !== undefined) signal?.removeEventListener('abort', abort);
  }
}

class OwnerLeaseRenewTimeoutError extends Error {
  constructor(timeoutMs: number) {
    super(`Owner lease renewal exceeded ${timeoutMs}ms.`);
    this.name = 'OwnerLeaseRenewTimeoutError';
  }
}

function remainingTimeoutMs(timeoutStartedAtMs: number, timeoutMs: number): number {
  return Math.max(0, timeoutMs - (performance.now() - timeoutStartedAtMs));
}

function errorMessage(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}
