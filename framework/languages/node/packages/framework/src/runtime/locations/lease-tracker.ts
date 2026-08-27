import {
  zlinkRuntimeDefaultLocationOptions,
  type ZLinkLocationOptionOverrides
} from '../../contracts/Locations/Options';
import {
  type ZLinkLocationOwnerToken,
  type ZLinkOwnerLeaseReadResult
} from './internal-location-contracts';
import type { ZLinkOwnerLeaseStore } from './internal-store-contracts';
import { AsyncResource } from 'node:async_hooks';
import { ZLinkStateLane } from '../execution/state-lane';

const detachedLeaseRefreshResource = new AsyncResource('zlink:owner-lease-tracker');

export interface ZLinkOwnerLeaseTrackerOptions {
  readonly store: ZLinkOwnerLeaseStore;
  readonly options?: ZLinkLocationOptionOverrides;
  readonly monotonicNowMs?: () => number;
}

interface OwnerLeaseTrackerSnapshot {
  readonly result: ZLinkOwnerLeaseReadResult;
  readonly fetchedAtMs: number;
}

interface OwnerLeaseRefresh {
  readonly promise: Promise<OwnerLeaseTrackerSnapshot>;
  readonly resolve: (snapshot: OwnerLeaseTrackerSnapshot) => void;
  readonly reject: (error: unknown) => void;
}

export class ZLinkOwnerLeaseTracker {
  private readonly lane = new ZLinkStateLane();
  private readonly store: ZLinkOwnerLeaseStore;
  private readonly options: Required<ZLinkLocationOptionOverrides>;
  private readonly monotonicNowMs: () => number;
  private readonly snapshots = new Map<string, OwnerLeaseTrackerSnapshot>();
  private readonly refreshes = new Map<string, OwnerLeaseRefresh>();
  private liveOwnerFingerprint?: string;
  private liveOwnerVersion = 0;

  constructor(options: ZLinkOwnerLeaseTrackerOptions) {
    this.store = options.store;
    this.options = { ...zlinkRuntimeDefaultLocationOptions, ...options.options };
    this.monotonicNowMs = options.monotonicNowMs ?? (() => performance.now());
  }

  async isOwnerLive(ownerId: string, signal?: AbortSignal): Promise<boolean> {
    const snapshot = await this.getSnapshot(ownerId, signal);
    return this.isLive(snapshot);
  }

  async remainingLeaseMs(ownerId: string, signal?: AbortSignal): Promise<number> {
    const snapshot = await this.getSnapshot(ownerId, signal);
    if (snapshot.result.kind !== 'found') return 0;
    return Math.max(0, snapshot.result.leaseExpiresAt.getTime()
      - snapshot.result.storeNow.getTime()
      - (this.monotonicNowMs() - snapshot.fetchedAtMs));
  }

  async remainingOwnerTokenLeaseMs(
    owner: ZLinkLocationOwnerToken,
    signal?: AbortSignal
  ): Promise<number> {
    const snapshot = await this.getSnapshot(owner.ownerId, signal);
    if (
      snapshot.result.kind !== 'found'
      || snapshot.result.token.leaseGeneration !== owner.leaseGeneration
    ) return 0;
    return Math.max(0, snapshot.result.leaseExpiresAt.getTime()
      - snapshot.result.storeNow.getTime()
      - (this.monotonicNowMs() - snapshot.fetchedAtMs));
  }

  async getLiveOwnerSetVersion(signal?: AbortSignal): Promise<number> {
    const owners = await this.lane.run(() => [...this.snapshots.keys()]);
    const refreshed = await Promise.all(owners.map(ownerId => this.getSnapshot(ownerId, signal)));
    const live = refreshed
      .filter(snapshot => this.isLive(snapshot))
      .map(snapshot => snapshot.result.kind === 'found' ? snapshot.result.token.ownerId : '')
      .filter(ownerId => ownerId.length > 0)
      .sort()
      .join('\n');
    return await this.lane.run(() => {
      if (live !== this.liveOwnerFingerprint) {
        this.liveOwnerFingerprint = live;
        this.liveOwnerVersion++;
      }
      return this.liveOwnerVersion;
    });
  }

  private async getSnapshot(
    ownerId: string,
    signal?: AbortSignal
  ): Promise<OwnerLeaseTrackerSnapshot> {
    const prepared = await this.lane.run(() => this.prepareSnapshotRefresh(ownerId));
    if (prepared.snapshot !== undefined) return prepared.snapshot;
    if (prepared.start) {
      void runDetachedLeaseRefresh(() => this.refreshSnapshot(ownerId, signal, prepared.refresh));
    }
    return await prepared.refresh.promise;
  }

  private prepareSnapshotRefresh(ownerId: string): {
    readonly snapshot?: OwnerLeaseTrackerSnapshot;
    readonly refresh: OwnerLeaseRefresh;
    readonly start: boolean;
  } {
    const current = this.snapshots.get(ownerId);
    if (current !== undefined
      && this.monotonicNowMs() - current.fetchedAtMs < this.options.pollingIntervalMs
      && this.isLive(current)) {
      return {
        snapshot: current,
        refresh: { promise: Promise.resolve(current), resolve: () => undefined, reject: () => undefined },
        start: false
      };
    }
    const activeRefresh = this.refreshes.get(ownerId);
    if (activeRefresh !== undefined) return { refresh: activeRefresh, start: false };

    let resolve!: (snapshot: OwnerLeaseTrackerSnapshot) => void;
    let reject!: (error: unknown) => void;
    const promise = new Promise<OwnerLeaseTrackerSnapshot>((complete, fail) => {
      resolve = complete;
      reject = fail;
    });
    const refresh = { promise, resolve, reject };
    this.refreshes.set(ownerId, refresh);
    return { refresh, start: true };
  }

  private async refreshSnapshot(
    ownerId: string,
    signal: AbortSignal | undefined,
    refresh: OwnerLeaseRefresh
  ): Promise<void> {
    try {
      const snapshot = {
        result: await this.store.readOwnerLease(ownerId, signal),
        fetchedAtMs: this.monotonicNowMs()
      };
      await this.lane.run(() => this.completeSnapshotRefresh(ownerId, refresh, snapshot));
    } catch (error) {
      await this.lane.run(() => this.failSnapshotRefresh(ownerId, refresh, error));
    }
  }

  private completeSnapshotRefresh(
    ownerId: string,
    refresh: OwnerLeaseRefresh,
    snapshot: OwnerLeaseTrackerSnapshot
  ): void {
    if (this.refreshes.get(ownerId) !== refresh) return;
    this.snapshots.set(ownerId, snapshot);
    this.refreshes.delete(ownerId);
    refresh.resolve(snapshot);
  }

  private failSnapshotRefresh(
    ownerId: string,
    refresh: OwnerLeaseRefresh,
    error: unknown
  ): void {
    if (this.refreshes.get(ownerId) !== refresh) return;
    this.refreshes.delete(ownerId);
    refresh.reject(error);
  }

  private isLive(snapshot: OwnerLeaseTrackerSnapshot): boolean {
    if (snapshot.result.kind !== 'found') return false;
    const elapsedMs = this.monotonicNowMs() - snapshot.fetchedAtMs;
    return snapshot.result.leaseExpiresAt.getTime()
      - snapshot.result.storeNow.getTime()
      - elapsedMs > 0;
  }
}

export class ZLinkLiveRowFilter {
  constructor(private readonly leaseTracker: ZLinkOwnerLeaseTracker) {}

  async filter<TRow>(
    rows: readonly TRow[],
    ownerIdOf: (row: TRow) => string,
    signal?: AbortSignal,
    include?: (row: TRow) => boolean
  ): Promise<TRow[]> {
    const live: TRow[] = [];
    for (const row of rows) {
      if ((include === undefined || include(row)) && await this.isLive(row, ownerIdOf, signal)) {
        live.push(row);
      }
    }
    return live;
  }

  async resolve<TRow>(
    row: TRow | undefined,
    ownerIdOf: (row: TRow) => string,
    signal?: AbortSignal,
    include?: (row: TRow) => boolean
  ): Promise<TRow | undefined> {
    if (row === undefined || (include !== undefined && !include(row))) {
      return undefined;
    }
    return await this.isLive(row, ownerIdOf, signal) ? row : undefined;
  }

  private async isLive<TRow>(
    row: TRow,
    ownerIdOf: (row: TRow) => string,
    signal?: AbortSignal
  ): Promise<boolean> {
    return await this.leaseTracker.isOwnerLive(ownerIdOf(row), signal);
  }
}

function runDetachedLeaseRefresh(work: () => Promise<void>): Promise<void> {
  return detachedLeaseRefreshResource.runInAsyncScope(work);
}
