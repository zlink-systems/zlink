import {
  zlinkRuntimeDefaultLocationOptions,
  type ZLinkLocationOptionOverrides
} from '../../contracts/Locations/Options';
import type { ZLinkDomainLocationStore as ZLinkLocationStore } from './domain-store-contract';
import type { ZLinkOwnerLeaseTracker } from './lease-tracker';
import type { ZLinkAutoConnectLocal } from './auto-connect-types';
import type { ZLinkAutoConnectReconciler } from './auto-connect-reconciler';
import { ZLinkStateLane } from '../execution/state-lane';

export interface ZLinkAutoConnectLoopOptions {
  readonly reconciler: ZLinkAutoConnectReconciler;
  readonly local: ZLinkAutoConnectLocal;
  readonly options?: ZLinkLocationOptionOverrides;
  readonly changeStampStore?: Pick<ZLinkLocationStore, 'getMeshNodeChangeStamp'>;
  readonly leaseTracker?: ZLinkOwnerLeaseTracker;
  readonly setTimer?: (callback: () => void, delayMs: number) => unknown;
  readonly clearTimer?: (handle: unknown) => void;
}

export class ZLinkAutoConnectLoop {
  private readonly lane = new ZLinkStateLane();
  private readonly reconciler: ZLinkAutoConnectReconciler;
  private readonly options: Required<ZLinkLocationOptionOverrides>;
  private readonly meshName: string;
  private readonly changeStampStore?: Pick<ZLinkLocationStore, 'getMeshNodeChangeStamp'>;
  private readonly leaseTracker?: ZLinkOwnerLeaseTracker;
  private readonly setTimer: (callback: () => void, delayMs: number) => unknown;
  private readonly clearTimer: (handle: unknown) => void;
  private controller?: AbortController;
  private timer?: unknown;
  private lastStamp?: bigint;
  private lastLiveOwnerSetVersion?: number;
  private lastTickFailed = false;
  // This settled tail orders whole operations; mutable state remains on the lane.
  private inFlightTick: Promise<void> = Promise.resolve();

  constructor(options: ZLinkAutoConnectLoopOptions) {
    this.reconciler = options.reconciler;
    this.options = { ...zlinkRuntimeDefaultLocationOptions, ...options.options };
    this.meshName = options.local.meshName;
    this.changeStampStore = options.changeStampStore;
    this.leaseTracker = options.leaseTracker;
    this.setTimer = options.setTimer ?? ((callback, delayMs) => setTimeout(callback, delayMs));
    this.clearTimer = options.clearTimer ?? ((handle) => clearTimeout(handle as NodeJS.Timeout));
  }

  async start(signal?: AbortSignal): Promise<void> {
    if (await this.lane.run(() => this.controller !== undefined)) return;
    await this.tick(signal);
    const started = await this.lane.run(() => {
      if (this.controller !== undefined) return false;
      this.controller = new AbortController();
      return true;
    });
    if (!started) return;
    this.scheduleNext();
  }

  async requestPeerReconcile(meshName: string, _peerRid: string): Promise<void> {
    const controller = this.controller;
    if (meshName !== this.meshName || controller === undefined) return;
    // A fresh descriptor scan uses the same RID ordering and connection owner as a periodic tick.
    await this.enqueueTick(async () => {
      if (!controller.signal.aborted) await this.runReconcile(controller.signal);
    });
  }

  async stop(signal?: AbortSignal): Promise<void> {
    await this.prepareTransportShutdown();
    await this.finishTransportShutdown(signal);
  }

  async prepareTransportShutdown(): Promise<void> {
    const controller = this.controller;
    this.controller = undefined;
    if (controller !== undefined) {
      controller.abort();
    }
    if (this.timer !== undefined) {
      this.clearTimer(this.timer);
      this.timer = undefined;
    }
    await this.inFlightTick;
    this.reconciler.disconnectPeers();
  }

  async finishTransportShutdown(signal?: AbortSignal): Promise<void> {
    await this.reconciler.unpublishLocal(signal);
  }

  async tick(signal?: AbortSignal): Promise<void> {
    const controller = this.controller;
    await this.enqueueTick(async () => {
      if (controller?.signal.aborted !== true) await this.tickCore(signal ?? controller?.signal);
    });
  }

  private enqueueTick(operation: () => Promise<void>): Promise<void> {
    const completion = this.inFlightTick.then(operation);
    // The caller receives the failure; the successor still owns its place in the order.
    this.inFlightTick = completion.then(
      () => undefined,
      () => undefined
    );
    return completion;
  }

  private async tickCore(signal?: AbortSignal): Promise<void> {
    const lastTickFailed = await this.lane.run(() => this.lastTickFailed);
    if (this.changeStampStore?.getMeshNodeChangeStamp !== undefined && !lastTickFailed) {
      try {
        const stamp = await this.changeStampStore.getMeshNodeChangeStamp(this.meshName, signal);
        const liveOwners =
          this.leaseTracker === undefined
            ? 0
            : await this.leaseTracker.getLiveOwnerSetVersion(signal);
        const publicationReady = this.reconciler.localPublicationReady;
        if (
          await this.lane.run(
            () =>
              this.lastStamp === stamp &&
              this.lastLiveOwnerSetVersion === liveOwners &&
              publicationReady
          )
        ) {
          return;
        }
        const tickFailed = await this.runReconcile(signal);
        if (!tickFailed) {
          await this.lane.run(() => {
            this.lastStamp = stamp;
            this.lastLiveOwnerSetVersion = liveOwners;
          });
        }
        return;
      } catch {
        // A failed stamp read degrades to a full reconcile tick.
      }
    }

    await this.runReconcile(signal);
    await this.lane.run(() => {
      this.lastStamp = undefined;
      this.lastLiveOwnerSetVersion = undefined;
    });
  }

  private async runReconcile(signal?: AbortSignal): Promise<boolean> {
    await this.reconciler.tick(signal);
    const failed = this.reconciler.storeFailed;
    return await this.lane.run(() => {
      this.lastTickFailed = failed;
      return failed;
    });
  }

  private scheduleNext(): void {
    if (this.controller === undefined || this.controller.signal.aborted) {
      return;
    }
    this.timer = this.setTimer(() => {
      this.timer = undefined;
      void this.tick(this.controller?.signal)
        .catch(() => undefined)
        .finally(() => this.scheduleNext());
    }, this.options.pollingIntervalMs);
  }
}
