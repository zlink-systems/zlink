import {
  zlinkRuntimeDefaultLocationOptions,
  type ZLinkLocationOptionOverrides
} from '../../contracts/Locations/Options';
import type { RoutingId } from '../../contracts/Common';
import {
  ZLinkLocationWriteIntent,
  ZLinkLocationWriteStatus,
  type ZLinkPeerLocationResolver,
  type ZLinkPeerLocation
} from './internal-location-contracts';
import { ZLinkLocationKeyCodec } from './key-codec';
import {
  ZLinkAutoConnectPlanner
} from './auto-connect-planner';
import type {
  IZLinkAutoConnectExecutor,
  IZLinkAutoConnectPeerPublisher,
  ZLinkAutoConnectEventSink,
  ZLinkAutoConnectLocal,
  ZLinkAutoConnectTarget
} from './auto-connect-types';

const encodeRoutingIdHex = ZLinkLocationKeyCodec.encodeRoutingIdHex;

export interface ZLinkAutoConnectReconcilerOptions {
  readonly local: ZLinkAutoConnectLocal;
  readonly localRow?: ZLinkPeerLocation;
  readonly runtime: IZLinkAutoConnectPeerPublisher;
  readonly peerResolver: ZLinkPeerLocationResolver;
  readonly executor: IZLinkAutoConnectExecutor;
  readonly reconcilePeers?: boolean;
  readonly events?: ZLinkAutoConnectEventSink;
  readonly options?: ZLinkLocationOptionOverrides;
  readonly monotonicNowMs?: () => number;
}

export class ZLinkAutoConnectReconciler {
  private readonly local: ZLinkAutoConnectLocal;
  private readonly localRow?: ZLinkPeerLocation;
  private readonly runtime: IZLinkAutoConnectPeerPublisher;
  private readonly peerResolver: ZLinkPeerLocationResolver;
  private readonly executor: IZLinkAutoConnectExecutor;
  private readonly reconcilePeers: boolean;
  private readonly events?: ZLinkAutoConnectEventSink;
  private readonly options: Required<ZLinkLocationOptionOverrides>;
  private readonly monotonicNowMs: () => number;
  private readonly active = new Map<string, ZLinkAutoConnectTarget>();
  private readonly pendingDisconnects = new Map<string, ZLinkAutoConnectTarget>();
  private readonly failedEndpoints = new Set<string>();
  private localGeneration = 0n;
  private localPublished = false;
  private storeFailedValue = false;
  private storeFailureStartedAtMs: number | undefined;
  private recoveryDeferUntilMs = 0;
  private lastDesired = new Map<string, ZLinkAutoConnectTarget>();
  private meshMemberRidHexes?: ReadonlySet<string>;

  constructor(options: ZLinkAutoConnectReconcilerOptions) {
    this.local = options.local;
    this.localRow = options.localRow;
    this.runtime = options.runtime;
    this.peerResolver = options.peerResolver;
    this.executor = options.executor;
    this.reconcilePeers = options.reconcilePeers ?? true;
    this.events = options.events;
    this.options = { ...zlinkRuntimeDefaultLocationOptions, ...options.options };
    this.monotonicNowMs = options.monotonicNowMs ?? (() => performance.now());
    this.executor.onDisconnected?.((endpoint) => this.recordDisconnectedEndpoint(endpoint));
  }

  get storeFailed(): boolean {
    return this.storeFailedValue;
  }

  get localPublicationReady(): boolean {
    return this.localPublished;
  }

  get activeTargets(): readonly ZLinkAutoConnectTarget[] {
    return [...this.active.values()];
  }

  knowsPeer(nodeRid: RoutingId): boolean | undefined {
    if (this.meshMemberRidHexes === undefined) {
      return undefined;
    }
    return this.meshMemberRidHexes.has(encodeRoutingIdHex(nodeRid));
  }

  async tick(signal?: AbortSignal): Promise<void> {
    try {
      await this.publishLocal(signal);
    } catch {
      this.recordStoreFailure();
      return;
    }

    if (!this.reconcilePeers) {
      this.storeFailedValue = false;
      this.storeFailureStartedAtMs = undefined;
      return;
    }

    let rows: readonly ZLinkPeerLocation[];
    try {
      rows = await this.peerResolver.listLivePeers({
        autoConnectType: this.local.autoConnectType,
        meshName: this.local.meshName
      }, signal);
    } catch {
      this.recordStoreFailure();
      return;
    }

    // A restarted Store can accept commands and return an empty live set
    // before the previous descriptor rows are rebuilt. In that case the
    // earlier publishLocal call did not observe an error, so localPublished
    // is still true even though this owner no longer has a row. Re-publish
    // only when the authoritative read proves that this local row is absent;
    // existing transport targets remain untouched during the rebuild window.
    if (
      this.localPublished
      && this.localRow !== undefined
      && !rows.some(row => row.nodeRid === this.localRow!.nodeRid)
    ) {
      this.localPublished = false;
      try {
        const republished = await this.publishLocal(signal);
        if (!republished) {
          this.recordStoreFailure();
          return;
        }
      } catch {
        this.recordStoreFailure();
        return;
      }
    }

    if (this.storeFailedValue) {
      this.storeFailedValue = false;
      this.storeFailureStartedAtMs = undefined;
      // After a store restart, owners may need one full lease interval to
      // reclaim their token and republish a descriptor. Do not interpret the
      // incomplete post-restart scan as a definitive removal.
      // Keep the existing transport set through one lease interval while
      // owners reclaim their tokens and republish descriptors. This deferral
      // applies only after a Store operation failed; a successful empty scan
      // is authoritative and must remove stale peers promptly.
      this.recoveryDeferUntilMs = this.monotonicNowMs()
        + this.options.ownerLeaseTtlMs;
    }

    this.meshMemberRidHexes = new Set(rows
      .map((row) => row.nodeRid)
      .filter((nodeRid): nodeRid is RoutingId => nodeRid !== undefined)
      .map((nodeRid) => encodeRoutingIdHex(nodeRid)));
    const candidates = ZLinkAutoConnectPlanner.computeCandidates(this.local, rows);
    const nowMs = this.monotonicNowMs();
    this.executor.expectPeers?.([...candidates.values()]);
    this.executor.replaceNotRequired?.(
      ZLinkAutoConnectPlanner.computeNotRequired(this.local, rows)
    );

    const desired = new Map(ZLinkAutoConnectPlanner.computeDesired(this.local, rows));
    const desiredEndpoints = new Set([...desired.values()].map((target) => target.endpoint));
    for (const endpoint of this.failedEndpoints) {
      if (!desiredEndpoints.has(endpoint)) {
        this.failedEndpoints.delete(endpoint);
      }
    }
    const existingTargets = ZLinkAutoConnectPlanner.computeDesired(this.local, rows, true);
    for (const [key, target] of existingTargets) {
      if (this.active.has(key)) desired.set(key, target);
    }
    this.lastDesired = new Map(desired);
    const connectedEndpoints: string[] = [];
    const disconnectedEndpoints: string[] = [];
    for (const [key, target] of desired) {
      let current = this.active.get(key);
      if (
        current !== undefined
        && current.endpoint === target.endpoint
        && current.ownerId === target.ownerId
        && this.executor.isDisconnected?.(current) === true
      ) {
        this.active.delete(key);
        current = undefined;
      }
      if (current === undefined) {
        const disconnecting = this.pendingDisconnects.get(key);
        if (disconnecting !== undefined
          && this.executor.isDisconnected !== undefined
          && !this.executor.isDisconnected(disconnecting)) {
          continue;
        }
        this.pendingDisconnects.delete(key);
        const connected = this.executor.connect(target);
        if (connected) {
          connectedEndpoints.push(target.endpoint);
          this.active.set(key, target);
          this.failedEndpoints.delete(target.endpoint);
        }
        continue;
      }

      if (current.endpoint !== target.endpoint || current.ownerId !== target.ownerId) {
        this.disconnectIfNeeded(current);
        disconnectedEndpoints.push(current.endpoint);
        this.active.delete(key);
        if (this.executor.isDisconnected !== undefined
          && !this.executor.isDisconnected(current)) {
          this.pendingDisconnects.set(key, current);
          continue;
        }
        this.pendingDisconnects.delete(key);
        const connected = this.executor.connect(target);
        if (connected) {
          connectedEndpoints.push(target.endpoint);
          this.active.set(key, target);
          this.failedEndpoints.delete(target.endpoint);
        }
      }
    }

    if (nowMs < this.recoveryDeferUntilMs) {
      this.publishDesiredSetChange(connectedEndpoints, disconnectedEndpoints);
      return;
    }

    // A store restart can expose an incomplete descriptor set while peers are
    // reclaiming their owner leases. Preserve existing transport connections
    // during that recovery window; stale-peer cleanup is safe after the
    // deferred reconciliation has completed.
    this.executor.disconnectStalePeers?.(
      [...candidates.values()]
    );

    for (const [key, target] of [...this.active]) {
      if (!desired.has(key)) {
        this.disconnectIfNeeded(target);
        disconnectedEndpoints.push(target.endpoint);
        this.active.delete(key);
        if (this.executor.isDisconnected !== undefined) {
          this.pendingDisconnects.set(key, target);
        }
      }
    }

    if (this.executor.isDisconnected !== undefined) {
      for (const [key, target] of this.pendingDisconnects) {
        if (!desired.has(key) && this.executor.isDisconnected(target)) {
          this.pendingDisconnects.delete(key);
        }
      }
    }

    this.publishDesiredSetChange(connectedEndpoints, disconnectedEndpoints);
  }

  async shutdown(signal?: AbortSignal): Promise<void> {
    this.disconnectPeers();
    await this.unpublishLocal(signal);
  }

  disconnectPeers(): void {
    for (const target of this.active.values()) {
      this.disconnectIfNeeded(target);
    }
    this.active.clear();
    this.pendingDisconnects.clear();
  }

  async unpublishLocal(signal?: AbortSignal): Promise<void> {
    if (this.localPublished && this.localRow !== undefined) {
      await this.runtime.removePeer({
        autoConnectType: this.localRow.autoConnectType,
        meshName: this.localRow.meshName,
        role: this.localRow.role,
        nodeRid: this.localRow.nodeRid,
        endpoint: this.localRow.endpoint
      }, this.localGeneration, signal);
      this.localPublished = false;
    }
  }

  private disconnectIfNeeded(target: ZLinkAutoConnectTarget): void {
    if (this.executor.isDisconnected?.(target) === true) return;
    this.executor.disconnect(target);
  }

  private async publishLocal(signal?: AbortSignal): Promise<boolean> {
    if (this.localRow === undefined || this.localPublished) {
      return this.localPublished;
    }

    const claimed = await this.runtime.writePeer(this.localRow, ZLinkLocationWriteIntent.NewClaim, signal);
    if (claimed.status === ZLinkLocationWriteStatus.Stored) {
      this.localGeneration = claimed.generation;
      this.localPublished = true;
      return true;
    }

    if (claimed.status === ZLinkLocationWriteStatus.RejectedConflict && this.localGeneration > 0n) {
      const renewed = await this.runtime.writePeer({
        ...this.localRow,
        generation: this.localGeneration
      }, ZLinkLocationWriteIntent.Renew, signal);
      this.localPublished = renewed.status === ZLinkLocationWriteStatus.Stored;
      return this.localPublished;
    }
    return false;
  }

  private recordStoreFailure(): void {
    const nowMs = this.monotonicNowMs();
    this.storeFailureStartedAtMs ??= nowMs;
    this.storeFailedValue = true;
    this.localPublished = false;
    if (
      this.options.storeFailureGraceMs <= 0 ||
      nowMs - this.storeFailureStartedAtMs > this.options.storeFailureGraceMs
    ) {
      return;
    }
    for (const [key, target] of this.lastDesired) {
      if (!this.active.has(key) && !this.failedEndpoints.has(target.endpoint) && this.executor.connect(target)) {
        this.active.set(key, target);
      }
    }
  }

  private recordDisconnectedEndpoint(endpoint: string): void {
    let removed = false;
    for (const [key, target] of this.active) {
      if (target.endpoint === endpoint) {
        this.active.delete(key);
        removed = true;
      }
    }
    if (removed) {
      this.failedEndpoints.add(endpoint);
    }
  }

  private publishDesiredSetChange(
    connectedEndpoints: readonly string[],
    disconnectedEndpoints: readonly string[]
  ): void {
    if (connectedEndpoints.length === 0 && disconnectedEndpoints.length === 0) {
      return;
    }
    this.events?.desiredSetChanged({
      autoConnectType: this.local.autoConnectType,
      meshName: this.local.meshName,
      connectedEndpoints,
      disconnectedEndpoints
    });
  }
}
