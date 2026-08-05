import {
  zlinkRuntimeDefaultLocationOptions,
  type ZLinkLocationOptionOverrides
} from '../../contracts/Locations/Options';
import {
  type ZLinkLocationOwnerToken,
  type ZLinkOwnerLeaseReadResult
} from './internal-location-contracts';
import type { ZLinkOwnerLeaseStore } from './internal-store-contracts';

export interface ZLinkOwnerLeaseTrackerOptions {
  readonly store: ZLinkOwnerLeaseStore;
  readonly options?: ZLinkLocationOptionOverrides;
  readonly monotonicNowMs?: () => number;
}

interface OwnerLeaseTrackerSnapshot {
  readonly result: ZLinkOwnerLeaseReadResult;
  readonly fetchedAtMs: number;
}

export class ZLinkOwnerLeaseTracker {
  private readonly store: ZLinkOwnerLeaseStore;
  private readonly options: Required<ZLinkLocationOptionOverrides>;
  private readonly monotonicNowMs: () => number;
  private readonly snapshots = new Map<string, OwnerLeaseTrackerSnapshot>();
  private readonly refreshes = new Map<string, Promise<OwnerLeaseTrackerSnapshot>>();
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
    const owners = [...this.snapshots.keys()];
    const refreshed = await Promise.all(owners.map(ownerId => this.getSnapshot(ownerId, signal)));
    const live = refreshed
      .filter(snapshot => this.isLive(snapshot))
      .map(snapshot => snapshot.result.kind === 'found' ? snapshot.result.token.ownerId : '')
      .filter(ownerId => ownerId.length > 0)
      .sort()
      .join('\n');
    if (live !== this.liveOwnerFingerprint) {
      this.liveOwnerFingerprint = live;
      this.liveOwnerVersion++;
    }
    return this.liveOwnerVersion;
  }

  private async getSnapshot(
    ownerId: string,
    signal?: AbortSignal
  ): Promise<OwnerLeaseTrackerSnapshot> {
    const current = this.snapshots.get(ownerId);
    if (current !== undefined
      && this.monotonicNowMs() - current.fetchedAtMs < this.options.pollingIntervalMs
      && this.isLive(current)) {
      return current;
    }

    const activeRefresh = this.refreshes.get(ownerId);
    if (activeRefresh !== undefined) {
      return activeRefresh;
    }

    const refresh = this.refreshSnapshot(ownerId, signal);
    this.refreshes.set(ownerId, refresh);
    try {
      return await refresh;
    } finally {
      this.refreshes.delete(ownerId);
    }
  }

  private async refreshSnapshot(
    ownerId: string,
    signal?: AbortSignal
  ): Promise<OwnerLeaseTrackerSnapshot> {
    const snapshot = {
      result: await this.store.readOwnerLease(ownerId, signal),
      fetchedAtMs: this.monotonicNowMs()
    };
    this.snapshots.set(ownerId, snapshot);
    return snapshot;
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
