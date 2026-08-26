import { AsyncResource } from 'node:async_hooks';
import { ZLinkStateLane } from '../execution/state-lane';

const detachedStateLaneResource = new AsyncResource('zlink:service-maintenance-runtime');

export type ServiceMaintenanceKind = 'retire' | 'shutdown';
export type ServiceMaintenanceState =
  | 'serving' | 'preparing' | 'retiring' | 'draining'
  | 'completed' | 'blocked' | 'forceStopped';

export interface ServiceRelocationUnit {
  readonly id: string;
  readonly ready: () => boolean;
  readonly relocate: (signal: AbortSignal) => Promise<void>;
}

export interface ServiceMaintenanceSnapshot {
  readonly kind?: ServiceMaintenanceKind;
  readonly state: ServiceMaintenanceState;
  readonly queued: number;
  readonly activeOutbound: number;
  readonly terminalError?: unknown;
}

export interface ServiceMaintenanceOptions {
  readonly preflight: (kind: ServiceMaintenanceKind, signal: AbortSignal) => Promise<boolean>;
  readonly publishState: (state: 'retiring' | 'draining') => void;
  readonly forceStop: () => Promise<void> | void;
}

/** Owns first-intent-wins maintenance admission and terminal observation. */
export class ServiceMaintenanceRuntime {
  private readonly lane = new ZLinkStateLane();
  private readonly units: ServiceRelocationUnit[] = [];
  private readonly observers = new Set<(snapshot: ServiceMaintenanceSnapshot) => void>();
  private activeOutbound = 0;
  private state: ServiceMaintenanceState = 'serving';
  private kind?: ServiceMaintenanceKind;
  private terminalError?: unknown;
  private operation?: Promise<ServiceMaintenanceSnapshot>;

  constructor(private readonly options: ServiceMaintenanceOptions) {}

  enqueue(unit: ServiceRelocationUnit): void {
    if (this.state !== 'serving' && this.state !== 'preparing' && this.state !== 'retiring') {
      throw new Error('Maintenance admission is sealed.');
    }
    if (unit.id.length === 0) {
      throw new TypeError('Relocation unit identity must be valid.');
    }
    this.units.push(unit);
    this.publish();
  }

  observe(observer: (snapshot: ServiceMaintenanceSnapshot) => void): () => void {
    this.observers.add(observer);
    observer(this.snapshot());
    return () => this.observers.delete(observer);
  }

  start(
    kind: ServiceMaintenanceKind,
    deadlineMs: number,
    stopStartingSignal?: AbortSignal,
    abortSignal?: AbortSignal
  ): Promise<ServiceMaintenanceSnapshot> {
    if (this.operation !== undefined) return this.operation;
    if (!Number.isFinite(deadlineMs) || deadlineMs < 0) {
      throw new RangeError('Maintenance deadline must be non-negative and finite.');
    }
    this.kind = kind;
    let resolve!: (snapshot: ServiceMaintenanceSnapshot) => void;
    let reject!: (error: unknown) => void;
    const operation = new Promise<ServiceMaintenanceSnapshot>((complete, fail) => {
      resolve = complete;
      reject = fail;
    });
    // Install first: publishing the synchronous preparing transition can
    // re-enter start(), which must observe this first intent rather than
    // create a second maintenance operation.
    this.operation = operation;
    void this.run(kind, deadlineMs, stopStartingSignal, abortSignal).then(resolve, reject);
    return operation;
  }

  snapshot(): ServiceMaintenanceSnapshot {
    return this.snapshotCore();
  }

  private snapshotCore(): ServiceMaintenanceSnapshot {
    return {
      ...(this.kind === undefined ? {} : { kind: this.kind }),
      state: this.state,
      queued: this.units.length,
      activeOutbound: this.activeOutbound,
      ...(this.terminalError === undefined ? {} : { terminalError: this.terminalError })
    };
  }

  private async run(
    kind: ServiceMaintenanceKind,
    deadlineMs: number,
    stopStartingSignal?: AbortSignal,
    abortSignal?: AbortSignal
  ): Promise<ServiceMaintenanceSnapshot> {
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(new Error('Maintenance deadline exceeded.')), deadlineMs);
    const abort = () => controller.abort(abortSignal?.reason);
    if (abortSignal?.aborted === true) {
      abort();
    } else {
      abortSignal?.addEventListener('abort', abort, { once: true });
    }
    try {
      this.transition('preparing');
      if (!await this.options.preflight(kind, controller.signal)) {
        return await this.transitionAsync('blocked');
      }
      if (kind === 'retire') {
        this.options.publishState('retiring');
        await this.transitionAsync('retiring');
        await this.runUnits(controller.signal, stopStartingSignal);
      }
      this.options.publishState('draining');
      await this.transitionAsync('draining');
      return await this.transitionAsync('completed');
    } catch (error) {
      const blocked = await this.lane.run(() => {
        this.terminalError = error;
        return this.state === 'preparing';
      });
      if (blocked) {
        return await this.transitionAsync('blocked');
      } else {
        await this.options.forceStop();
        return await this.transitionAsync('forceStopped');
      }
    } finally {
      clearTimeout(timer);
      abortSignal?.removeEventListener('abort', abort);
    }
  }

  private async runUnits(
    signal: AbortSignal,
    stopStartingSignal?: AbortSignal
  ): Promise<void> {
    const pending = new Set<Promise<void>>();
    for (;;) {
      const queued = await this.lane.run(() => this.units.length > 0);
      if (!queued && pending.size === 0) return;
      signal.throwIfAborted();
      if (stopStartingSignal?.aborted === true) {
        await Promise.allSettled(pending);
        stopStartingSignal.throwIfAborted();
      }
      let admitted = false;
      for (let index = 0;;) {
        if (stopStartingSignal?.aborted === true) break;
        const unit = await this.lane.run(() => this.units.at(index));
        if (unit === undefined) break;
        if (!unit.ready()) {
          index++;
          continue;
        }
        const claim = await this.lane.run(() => {
          const current = this.units.indexOf(unit);
          if (current < 0) return { claimed: false };
          this.units.splice(current, 1);
          this.activeOutbound++;
          return { claimed: true, publication: this.publishCore() };
        });
        if (!claim.claimed || claim.publication === undefined) continue;
        admitted = true;
        let running!: Promise<void>;
        running = startOutsideStateLane(() => unit.relocate(signal).finally(async () => {
          const publication = await this.lane.run(() => {
            this.activeOutbound--;
            pending.delete(running);
            return this.publishCore();
          });
          this.notify(publication);
        }));
        pending.add(running);
        this.notify(claim.publication);
      }
      if (pending.size === 0) {
        if (!admitted) throw new Error('No relocation unit is ready to start.');
      } else {
        await Promise.race(pending);
      }
    }
  }

  private transition(state: ServiceMaintenanceState): void {
    this.state = state;
    this.publish();
  }

  private async transitionAsync(state: ServiceMaintenanceState): Promise<ServiceMaintenanceSnapshot> {
    const publication = await this.lane.run(() => {
      this.state = state;
      return this.publishCore();
    });
    this.notify(publication);
    return publication.snapshot;
  }

  private publish(): void {
    this.notify(this.publishCore());
  }

  private publishCore(): {
    readonly snapshot: ServiceMaintenanceSnapshot;
    readonly observers: readonly ((snapshot: ServiceMaintenanceSnapshot) => void)[];
  } {
    return { snapshot: this.snapshotCore(), observers: [...this.observers] };
  }

  private notify(publication: {
    readonly snapshot: ServiceMaintenanceSnapshot;
    readonly observers: readonly ((snapshot: ServiceMaintenanceSnapshot) => void)[];
  }): void {
    for (const observer of publication.observers) observer(publication.snapshot);
  }
}

function startOutsideStateLane<T>(work: () => T): T {
  return detachedStateLaneResource.runInAsyncScope(work);
}

export function classifyRelocationRecovery(
  publishedReference: boolean,
  payloadPresent: boolean,
  checksumMatches: boolean,
  inventoryMatches: boolean
): 'resume' | 'orphan' | 'relocationDataLost' {
  if (!publishedReference) return payloadPresent ? 'orphan' : 'resume';
  return payloadPresent && checksumMatches && inventoryMatches
    ? 'resume'
    : 'relocationDataLost';
}
