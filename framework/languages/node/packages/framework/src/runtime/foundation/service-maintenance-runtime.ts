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
    this.operation = this.run(kind, deadlineMs, stopStartingSignal, abortSignal);
    return this.operation;
  }

  snapshot(): ServiceMaintenanceSnapshot {
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
        this.transition('blocked');
        return this.snapshot();
      }
      if (kind === 'retire') {
        this.options.publishState('retiring');
        this.transition('retiring');
        await this.runUnits(controller.signal, stopStartingSignal);
      }
      this.options.publishState('draining');
      this.transition('draining');
      this.transition('completed');
      return this.snapshot();
    } catch (error) {
      this.terminalError = error;
      if (this.state === 'preparing') {
        this.transition('blocked');
      } else {
        await this.options.forceStop();
        this.transition('forceStopped');
      }
      return this.snapshot();
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
    while (this.units.length > 0 || pending.size > 0) {
      signal.throwIfAborted();
      if (stopStartingSignal?.aborted === true) {
        await Promise.allSettled(pending);
        stopStartingSignal.throwIfAborted();
      }
      let admitted = false;
      for (let index = 0; index < this.units.length;) {
        if (stopStartingSignal?.aborted === true) break;
        const unit = this.units[index]!;
        if (!unit.ready()) {
          index++;
          continue;
        }
        this.units.splice(index, 1);
        admitted = true;
        this.activeOutbound++;
        const running = unit.relocate(signal).finally(() => {
          this.activeOutbound--;
          pending.delete(running);
          this.publish();
        });
        pending.add(running);
        this.publish();
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

  private publish(): void {
    const snapshot = this.snapshot();
    for (const observer of this.observers) observer(snapshot);
  }
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
