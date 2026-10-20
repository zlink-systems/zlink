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
    await this.lane.run(async () => {
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
    this.reconciler.disconnectPeers();
  }

  async finishTransportShutdown(signal?: AbortSignal): Promise<void> {
    await this.reconciler.unpublishLocal(signal);
  }

  async tick(signal?: AbortSignal): Promise<void> {
    await this.lane.run(() => this.tickCore(signal));
  }

  private async tickCore(signal?: AbortSignal): Promise<void> {
    const lastTickFailed = this.lastTickFailed;
    if (this.changeStampStore?.getMeshNodeChangeStamp !== undefined && !lastTickFailed) {
      try {
        const stamp = await this.changeStampStore.getMeshNodeChangeStamp(this.meshName, signal);
        const liveOwners =
          this.leaseTracker === undefined
            ? 0
            : await this.leaseTracker.getLiveOwnerSetVersion(signal);
        if (
          this.lastStamp === stamp &&
          this.lastLiveOwnerSetVersion === liveOwners &&
          this.reconciler.localPublicationReady
        ) {
          return;
        }
        const tickFailed = await this.runReconcile(signal);
        if (!tickFailed) {
          this.lastStamp = stamp;
          this.lastLiveOwnerSetVersion = liveOwners;
        }
        return;
      } catch {
        // A failed stamp read degrades to a full reconcile tick.
      }
    }

    await this.runReconcile(signal);
    this.lastStamp = undefined;
    this.lastLiveOwnerSetVersion = undefined;
  }

  private async runReconcile(signal?: AbortSignal): Promise<boolean> {
    await this.reconciler.tick(signal);
    this.lastTickFailed = this.reconciler.storeFailed;
    return this.lastTickFailed;
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
