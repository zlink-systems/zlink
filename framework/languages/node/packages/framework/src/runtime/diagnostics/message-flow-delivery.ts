import { Worker } from 'node:worker_threads';

// The process owner counts records until the worker takes them, including batches
// already posted to its port. Only the worker decrements in-transit records.
export const MESSAGE_FLOW_PENDING_LIMIT = 8192;
export const TELEMETRY_WORKER_AVAILABLE = 'zlink.telemetry.available';
export enum TelemetryState {
  Pending,
  Enabled,
  Length
}
export enum TelemetryCounter {
  Overflow,
  LoggerFailures,
  TraceFailures,
  FallbackFailures,
  WorkerFailures,
  Length
}
export const TELEMETRY_STATE_BYTES = Math.ceil(
  TelemetryState.Length * Int32Array.BYTES_PER_ELEMENT / BigUint64Array.BYTES_PER_ELEMENT
) * BigUint64Array.BYTES_PER_ELEMENT;

export interface ZLinkTelemetryLease {
  readonly retired: Int32Array;
}

export interface ZLinkTelemetryDelivery<T = unknown> {
  readonly lease: SharedArrayBuffer;
  readonly record: T;
}

export function openTelemetryLease(): ZLinkTelemetryLease {
  return { retired: new Int32Array(new SharedArrayBuffer(Int32Array.BYTES_PER_ELEMENT)) };
}

export function renewTelemetryLease(lease: ZLinkTelemetryLease | undefined): ZLinkTelemetryLease {
  return lease === undefined || Atomics.load(lease.retired, 0) !== 0
    ? openTelemetryLease() : lease;
}

let processOwner: ZLinkMessageFlowDelivery | undefined;

export function messageFlowDelivery(): ZLinkMessageFlowDelivery {
  return (processOwner ??= new ZLinkMessageFlowDelivery());
}

export function telemetryProviderFailureCount(): number {
  return processOwner === undefined ? 0
    : Number(Atomics.load(processOwner.counters, TelemetryCounter.LoggerFailures)
      + Atomics.load(processOwner.counters, TelemetryCounter.TraceFailures));
}

export function retireTelemetryLease(lease: ZLinkTelemetryLease): void {
  Atomics.store(lease.retired, 0, 1);
  processOwner?.retire(lease);
}

export class ZLinkMessageFlowDelivery {
  private readonly sharedState = new SharedArrayBuffer(TELEMETRY_STATE_BYTES + TelemetryCounter.Length * BigUint64Array.BYTES_PER_ELEMENT);
  readonly state = new Int32Array(this.sharedState, 0, TelemetryState.Length);
  readonly counters = new BigUint64Array(this.sharedState, TELEMETRY_STATE_BYTES, TelemetryCounter.Length);
  readonly ready: Promise<void>;
  private readonly worker?: Worker;
  private failure?: Error;
  private pending: ZLinkTelemetryDelivery[] = [];
  private executing = false;
  private workerReady = false;
  private scheduled?: NodeJS.Immediate;

  constructor() {
    let published!: () => void;
    // A failed bootstrap leaves this incomplete. Runtime processing never waits
    // for it, and no rejected promise can escape into the host process.
    this.ready = new Promise(resolve => { published = resolve; });
    // NODE_OPTIONS preloads precede execArgv preloads. Keep both application
    // lists intact and capture the public default provider before either list.
    try {
      const bootstrap = require.resolve('./message-flow-bootstrap').replace(/\\/g, '/');
      this.worker = new Worker(require.resolve('./message-flow-worker'), {
      workerData: this.state.buffer,
      env: {
        ...process.env,
        NODE_OPTIONS: `--require "${bootstrap}" ${process.env.NODE_OPTIONS ?? ''}`
      }
    });
      this.worker.once('error', error => this.workerFailed(error));
      this.worker.once('exit', code => {
        if (this.failure === undefined) this.workerFailed(new Error(`Telemetry worker exited with code ${code}.`));
      });
      this.worker.on('message', (message: unknown) => {
        if (message !== TELEMETRY_WORKER_AVAILABLE || this.failure !== undefined) return;
        this.workerReady = true;
        this.executing = false;
        this.flush();
        published();
      });
      this.worker.unref();
    } catch (error) {
      this.workerFailed(error instanceof Error ? error : new Error(String(error)));
    }
  }

  private workerFailed(error: Error): void {
    if (this.failure !== undefined) return;
    this.failure = error;
    Atomics.add(this.counters, TelemetryCounter.WorkerFailures, 1n);
    Atomics.store(this.state, TelemetryState.Enabled, 0);
    this.workerReady = false;
    this.executing = false;
    this.pending = [];
    Atomics.store(this.state, TelemetryState.Pending, 0);
  }

  enabled(): boolean {
    return Atomics.load(this.state, TelemetryState.Enabled) !== 0;
  }

  accepts(lease: ZLinkTelemetryLease): boolean {
    return this.enabled() && Atomics.load(lease.retired, 0) === 0;
  }

  submit(lease: ZLinkTelemetryLease, record: unknown): void {
    if (!this.accepts(lease)) return;
    if (Atomics.load(this.state, TelemetryState.Pending) === MESSAGE_FLOW_PENDING_LIMIT) {
      Atomics.add(this.counters, TelemetryCounter.Overflow, 1n);
      return;
    }
    Atomics.add(this.state, TelemetryState.Pending, 1);
    this.pending.push({ lease: lease.retired.buffer as SharedArrayBuffer, record });
    if (this.workerReady && !this.executing && this.scheduled === undefined) {
      this.scheduled = setImmediate(() => {
        this.scheduled = undefined;
        this.flush();
      });
      this.scheduled.unref();
    }
  }

  retire(lease: ZLinkTelemetryLease): void {
    const retained = this.pending.filter((item) => item.lease !== lease.retired.buffer);
    Atomics.sub(this.state, TelemetryState.Pending, this.pending.length - retained.length);
    this.pending = retained;
  }

  private flush(): void {
    if (this.worker === undefined || !this.workerReady || this.executing || this.pending.length === 0) return;
    const batch = this.pending;
    this.pending = [];
    this.executing = true;
    this.worker.postMessage(batch);
  }
}
