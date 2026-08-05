import { ZLinkFrameworkInternalErrorKind, createInternalFrameworkException } from '../framework-errors-internal';
import { Worker } from 'node:worker_threads';
import type { ZLinkWorkerCall } from '../../contracts';
import { ZLinkFrameworkException } from '../../contracts';
import type { ZLinkWorkerOptions } from '../configuration';
import { ZLinkConfigurationException } from '../configuration';
import {
  defaultWorkerMaxThreads,
  DEFAULT_WORKER_IDLE_TIMEOUT_MS,
  DEFAULT_WORKER_MIN_THREADS,
  DEFAULT_WORKER_QUEUE_LENGTH
} from '../../contracts/Configuration/InternalDefaults';
import { createAbortError } from '../abort';
import {
  captureZLinkSpotSerialTurn,
  requireZLinkYieldTurn,
  type ZLinkSpotSerialTurn
} from '../execution';

export type ZLinkCpuWorkerWork<T> = (signal: AbortSignal) => T;
export type ZLinkIoWorkerWork<T> = (signal: AbortSignal) => Promise<T>;

export interface ZLinkWorkerRuntimeOptions {
  readonly minThreads: number;
  readonly maxThreads: number;
  readonly idleTimeoutMs: number;
  readonly maxQueueLength: number;
}

export function resolveWorkerRuntimeOptions(options?: ZLinkWorkerOptions): ZLinkWorkerRuntimeOptions {
  const resolved = {
    minThreads: options?.minThreads ?? DEFAULT_WORKER_MIN_THREADS,
    maxThreads: options?.maxThreads ?? defaultWorkerMaxThreads(),
    idleTimeoutMs: options?.idleTimeoutMs ?? DEFAULT_WORKER_IDLE_TIMEOUT_MS,
    maxQueueLength: options?.maxQueueLength ?? DEFAULT_WORKER_QUEUE_LENGTH
  };
  requireNonNegativeInteger('Worker minThreads', resolved.minThreads);
  requirePositiveInteger('Worker maxThreads', resolved.maxThreads);
  requireNonNegativeInteger('Worker idleTimeoutMs', resolved.idleTimeoutMs);
  requirePositiveInteger('Worker maxQueueLength', resolved.maxQueueLength);
  if (resolved.maxThreads < resolved.minThreads) {
    throw new ZLinkConfigurationException('Worker maxThreads must be greater than or equal to minThreads.');
  }
  return resolved;
}

interface ZLinkCpuJob<T> {
  readonly source: string;
  readonly timeoutMs?: number;
  readonly signal?: AbortSignal;
  readonly resolve: (value: T) => void;
  readonly reject: (error: unknown) => void;
  settled: boolean;
  running: boolean;
  abortListener?: () => void;
  activeAbortListener?: () => void;
  timeout?: ReturnType<typeof setTimeout>;
  slot?: ZLinkCpuWorkerSlot;
  abortState?: Int32Array;
}

interface ZLinkCpuWorkerSlot {
  readonly worker: Worker;
  job?: ZLinkCpuJob<unknown>;
  idleTimer?: ReturnType<typeof setTimeout>;
  terminating: boolean;
}

/** Runs synchronous CPU functions in an elastic, bounded set of worker threads. */
class ZLinkCpuWorkerPool {
  private readonly queue: Array<ZLinkCpuJob<unknown> | undefined> = [];
  private readonly slots = new Set<ZLinkCpuWorkerSlot>();
  private queueHead = 0;
  private queueCount = 0;
  private inFlight = 0;
  private pumping = false;

  constructor(private readonly options: ZLinkWorkerRuntimeOptions) {
    for (let index = 0; index < options.minThreads; index += 1) {
      this.createSlot();
    }
  }

  get pendingCount(): number {
    return this.queueCount;
  }

  get inFlightCount(): number {
    return this.inFlight;
  }

  schedule<T>(work: ZLinkCpuWorkerWork<T>, timeoutMs?: number, signal?: AbortSignal): Promise<T> {
    if (work.constructor.name === 'AsyncFunction') {
      return Promise.reject(new ZLinkConfigurationException(
        'runCpuWorker requires a synchronous function; use runIoWorker for async work.'
      ));
    }
    if (signal?.aborted === true) {
      return Promise.reject(createAbortError());
    }
    if (this.queueCount >= this.options.maxQueueLength) {
      return Promise.reject(workerQueueFull(this.options.maxQueueLength));
    }
    return new Promise<T>((resolve, reject) => {
      const job = {
        source: work.toString(),
        timeoutMs,
        signal,
        resolve,
        reject,
        settled: false,
        running: false
      } as ZLinkCpuJob<unknown>;
      if (signal !== undefined) {
        job.abortListener = () => {
          if (job.settled || job.running) return;
          job.settled = true;
          this.removeQueuedJob(job);
          signal.removeEventListener('abort', job.abortListener!);
          reject(createAbortError());
          this.pump();
        };
        signal.addEventListener('abort', job.abortListener, { once: true });
        if (signal.aborted) {
          job.abortListener();
          return;
        }
      }
      this.queue.push(job);
      this.queueCount += 1;
      this.pump();
    });
  }

  private pump(): void {
    if (this.pumping) return;
    this.pumping = true;
    try {
      this.ensureMinimumSlots();
      while (this.queueCount > 0) {
        const slot = this.findIdleSlot() ?? (
          this.slots.size < this.options.maxThreads ? this.createSlot() : undefined
        );
        if (slot === undefined) return;
        const job = this.takeQueuedJob();
        if (job === undefined) return;
        if (job.settled) continue;
        this.assign(slot, job);
      }
    } finally {
      this.pumping = false;
    }
  }

  private ensureMinimumSlots(): void {
    while (this.slots.size < this.options.minThreads) {
      this.createSlot();
    }
  }

  private findIdleSlot(): ZLinkCpuWorkerSlot | undefined {
    for (const slot of this.slots) {
      if (!slot.terminating && slot.job === undefined) return slot;
    }
    return undefined;
  }

  private createSlot(): ZLinkCpuWorkerSlot {
    const worker = new Worker(CPU_WORKER_SOURCE, { eval: true });
    const slot: ZLinkCpuWorkerSlot = { worker, terminating: false };
    this.slots.add(slot);
    worker.on('message', (message: CpuWorkerMessage) => this.onWorkerMessage(slot, message));
    worker.on('error', (error) => this.onWorkerError(slot, error));
    worker.on('exit', (code) => this.onWorkerExit(slot, code));
    // Keep baseline slots from retaining an otherwise idle host process.
    worker.unref();
    return slot;
  }

  private assign(slot: ZLinkCpuWorkerSlot, job: ZLinkCpuJob<unknown>): void {
    slot.job = job;
    job.slot = slot;
    job.running = true;
    // An active job must keep the event loop open until its result arrives.
    slot.worker.ref();
    job.abortState = new Int32Array(new SharedArrayBuffer(Int32Array.BYTES_PER_ELEMENT));
    this.inFlight += 1;
    if (slot.idleTimer !== undefined) {
      clearTimeout(slot.idleTimer);
      slot.idleTimer = undefined;
    }
    if (job.timeoutMs !== undefined) {
      job.timeout = setTimeout(() => {
        this.cancelRunningJob(slot, job, workerTimedOut(job.timeoutMs as number));
      }, job.timeoutMs);
    }
    if (job.signal !== undefined) {
      job.activeAbortListener = () => {
        this.cancelRunningJob(slot, job, createAbortError());
      };
      job.signal.addEventListener('abort', job.activeAbortListener, { once: true });
      if (job.signal.aborted) {
        job.activeAbortListener();
        return;
      }
    }
    try {
      slot.worker.postMessage({
        source: job.source,
        abortState: job.abortState.buffer
      });
    } catch (error) {
      this.terminateSlot(slot);
      this.finishJob(slot, job, () => job.reject(workerFailed(error)), false);
    }
  }

  private onWorkerMessage(slot: ZLinkCpuWorkerSlot, message: CpuWorkerMessage): void {
    const job = slot.job;
    if (job === undefined || job.settled) return;
    if (message.ok) {
      this.finishJob(slot, job, () => job.resolve(message.value), true);
    } else {
      this.finishJob(
        slot,
        job,
        () => job.reject(workerFailed(deserializeWorkerError(message.error))),
        true
      );
    }
  }

  private onWorkerError(slot: ZLinkCpuWorkerSlot, error: Error): void {
    slot.terminating = true;
    const job = slot.job;
    if (job !== undefined && !job.settled) {
      this.finishJob(slot, job, () => job.reject(workerFailed(error)), false);
    }
  }

  private onWorkerExit(slot: ZLinkCpuWorkerSlot, code: number): void {
    if (slot.idleTimer !== undefined) clearTimeout(slot.idleTimer);
    slot.idleTimer = undefined;
    this.slots.delete(slot);
    const job = slot.job;
    if (job !== undefined && !job.settled) {
      this.finishJob(
        slot,
        job,
        () => job.reject(workerFailed(new Error(`CPU worker exited with code ${code}.`))),
        false
      );
    }
    this.pump();
  }

  private cancelRunningJob(slot: ZLinkCpuWorkerSlot, job: ZLinkCpuJob<unknown>, error: unknown): void {
    if (job.settled || slot.job !== job) return;
    if (job.abortState !== undefined) Atomics.store(job.abortState, 0, 1);
    this.terminateSlot(slot);
    this.finishJob(slot, job, () => job.reject(error), false);
  }

  private terminateSlot(slot: ZLinkCpuWorkerSlot): void {
    if (slot.terminating) return;
    slot.terminating = true;
    void slot.worker.terminate().catch(() => undefined);
  }

  private finishJob(
    slot: ZLinkCpuWorkerSlot,
    job: ZLinkCpuJob<unknown>,
    complete: () => void,
    keepSlot: boolean
  ): void {
    if (job.settled) return;
    job.settled = true;
    job.running = false;
    if (job.timeout !== undefined) clearTimeout(job.timeout);
    if (job.signal !== undefined) {
      if (job.abortListener !== undefined) job.signal.removeEventListener('abort', job.abortListener);
      if (job.activeAbortListener !== undefined) {
        job.signal.removeEventListener('abort', job.activeAbortListener);
      }
    }
    job.timeout = undefined;
    job.abortState = undefined;
    job.slot = undefined;
    slot.job = undefined;
    this.inFlight -= 1;
    complete();
    if (keepSlot && !slot.terminating) {
      this.scheduleIdleTermination(slot);
      slot.worker.unref();
    }
    this.pump();
  }

  private scheduleIdleTermination(slot: ZLinkCpuWorkerSlot): void {
    if (slot.terminating || slot.job !== undefined || this.slots.size <= this.options.minThreads) return;
    if (slot.idleTimer !== undefined) clearTimeout(slot.idleTimer);
    slot.idleTimer = setTimeout(() => {
      slot.idleTimer = undefined;
      if (slot.job === undefined && !slot.terminating && this.slots.size > this.options.minThreads) {
        this.terminateSlot(slot);
      }
    }, this.options.idleTimeoutMs);
    slot.idleTimer.unref();
  }

  private takeQueuedJob(): ZLinkCpuJob<unknown> | undefined {
    while (this.queueHead < this.queue.length && this.queue[this.queueHead] === undefined) {
      this.queueHead += 1;
    }
    const job = this.queue[this.queueHead];
    if (job === undefined) return undefined;
    this.queue[this.queueHead] = undefined;
    this.queueHead += 1;
    this.queueCount -= 1;
    if (this.queueCount === 0) {
      this.queue.length = 0;
      this.queueHead = 0;
    } else if (this.queueHead >= 1024 && this.queueHead * 2 >= this.queue.length) {
      this.queue.splice(0, this.queueHead);
      this.queueHead = 0;
    }
    return job;
  }

  private removeQueuedJob(job: ZLinkCpuJob<unknown>): void {
    for (let index = this.queueHead; index < this.queue.length; index += 1) {
      if (this.queue[index] !== job) continue;
      this.queue[index] = undefined;
      this.queueCount -= 1;
      if (this.queueCount === 0) {
        this.queue.length = 0;
        this.queueHead = 0;
      } else {
        while (this.queueHead < this.queue.length && this.queue[this.queueHead] === undefined) {
          this.queueHead += 1;
        }
        if (this.queueHead >= 1024 && this.queueHead * 2 >= this.queue.length) {
          this.queue.splice(0, this.queueHead);
          this.queueHead = 0;
        }
      }
      return;
    }
  }

}
interface CpuWorkerMessage {
  readonly ok: boolean;
  readonly value?: unknown;
  readonly error?: { readonly name?: string; readonly message?: string; readonly stack?: string };
}

const CPU_WORKER_SOURCE = String.raw`
const { parentPort } = require('node:worker_threads');
parentPort.on('message', ({ source, abortState }) => {
  const state = new Int32Array(abortState);
  const signal = {
    get aborted() { return Atomics.load(state, 0) !== 0; },
    get reason() { return this.aborted ? new DOMException('The operation was aborted.', 'AbortError') : undefined; },
    throwIfAborted() { if (this.aborted) throw this.reason; },
    addEventListener() {},
    removeEventListener() {},
    dispatchEvent() { return false; },
    onabort: null
  };
  try {
    const work = (0, eval)('(' + source + ')');
    const value = work(signal);
    if (value !== null && (typeof value === 'object' || typeof value === 'function') && typeof value.then === 'function') {
      throw new TypeError('runCpuWorker function returned a Promise; use runIoWorker for async work.');
    }
    parentPort.postMessage({ ok: true, value });
  } catch (error) {
    parentPort.postMessage({
      ok: false,
      error: {
        name: error && error.name,
        message: error && error.message ? error.message : String(error),
        stack: error && error.stack
      }
    });
  }
});
`;

/** Runs asynchronous I/O functions without consuming a CPU worker thread. */
class ZLinkIoWorkerRuntime {
  schedule<T>(work: ZLinkIoWorkerWork<T>, timeoutMs?: number, signal?: AbortSignal): Promise<T> {
    if (signal?.aborted === true) {
      return Promise.reject(createAbortError());
    }
    return new Promise<T>((resolve, reject) => {
      const controller = new AbortController();
      let settled = false;
      let timeout: ReturnType<typeof setTimeout> | undefined;
      let abortListener: (() => void) | undefined;
      const settle = (complete: () => void): void => {
        if (settled) return;
        settled = true;
        if (timeout !== undefined) clearTimeout(timeout);
        if (signal !== undefined && abortListener !== undefined) {
          signal.removeEventListener('abort', abortListener);
        }
        complete();
      };
      if (timeoutMs !== undefined) {
        timeout = setTimeout(() => {
          controller.abort();
          settle(() => reject(workerTimedOut(timeoutMs)));
        }, timeoutMs);
      }
      if (signal !== undefined) {
        abortListener = () => {
          controller.abort();
          settle(() => reject(createAbortError()));
        };
        signal.addEventListener('abort', abortListener, { once: true });
      }
      Promise.resolve().then(() => work(controller.signal)).then(
        (value) => settle(() => resolve(value)),
        (error) => settle(() => reject(workerFailed(error)))
      );
    });
  }
}

export class ZLinkWorkerRuntime {
  private readonly cpu: ZLinkCpuWorkerPool;
  private readonly io = new ZLinkIoWorkerRuntime();

  constructor(options?: ZLinkWorkerOptions) {
    this.cpu = new ZLinkCpuWorkerPool(resolveWorkerRuntimeOptions(options));
  }

  get pendingCount(): number {
    return this.cpu.pendingCount;
  }

  get inFlightCount(): number {
    return this.cpu.inFlightCount;
  }

  scheduleCpu<T>(work: ZLinkCpuWorkerWork<T>, timeoutMs?: number, signal?: AbortSignal): Promise<T> {
    return this.cpu.schedule(work, timeoutMs, signal);
  }

  scheduleIo<T>(work: ZLinkIoWorkerWork<T>, timeoutMs?: number, signal?: AbortSignal): Promise<T> {
    return this.io.schedule(work, timeoutMs, signal);
  }
}

export interface ZLinkSpotSerialLike {
  execute<T>(operation: () => Promise<T> | T): Promise<T>;
  post<T>(operation: () => Promise<T> | T): Promise<T>;
  yieldPromise<T>(pending: Promise<T>): Promise<T>;
}

export class DefaultZLinkWorkerCall<T> implements ZLinkWorkerCall<T> {
  private selectedTimeoutMs: number | undefined;
  private terminatorSelected = false;
  private readonly turn: ZLinkSpotSerialTurn | undefined;

  constructor(
    private readonly serial: ZLinkSpotSerialLike,
    private readonly schedule: (timeoutMs?: number, signal?: AbortSignal) => Promise<T>
  ) {
    this.turn = captureZLinkSpotSerialTurn();
  }

  timeoutMs(durationMs: number): ZLinkWorkerCall<T> {
    this.selectedTimeoutMs = durationMs;
    return this;
  }

  submit(signal?: AbortSignal): Promise<T> {
    const pending = this.begin(signal);
    return this.turn === undefined ? deliverOnSerial(this.serial, pending) : pending;
  }

  yield(signal?: AbortSignal): Promise<T> {
    const turn = requireZLinkYieldTurn(this.turn);
    const pending = this.begin(signal);
    return turn.yieldPromise(pending);
  }

  private begin(signal?: AbortSignal): Promise<T> {
    if (this.terminatorSelected) {
      throw new ZLinkConfigurationException('A worker call can select only one terminator.');
    }
    this.terminatorSelected = true;
    return this.schedule(this.selectedTimeoutMs, signal);
  }
}

export function deliverOnSerial<T>(serial: ZLinkSpotSerialLike, pending: Promise<T>): Promise<T> {
  return new ZLinkSerialDeliveredPromise(serial, pending);
}

class ZLinkSerialDeliveredPromise<T> implements Promise<T> {
  readonly [Symbol.toStringTag] = 'Promise';

  constructor(private readonly serial: ZLinkSpotSerialLike, private readonly pending: Promise<T>) {}

  then<TResult1 = T, TResult2 = never>(
    onfulfilled?: ((value: T) => TResult1 | PromiseLike<TResult1>) | null,
    onrejected?: ((reason: unknown) => TResult2 | PromiseLike<TResult2>) | null
  ): Promise<TResult1 | TResult2> {
    return this.pending.then(
      (value) => this.serial.execute(() => onfulfilled === undefined || onfulfilled === null
        ? value as unknown as TResult1
        : Promise.resolve(onfulfilled(value))),
      (reason) => this.serial.execute(() => {
        if (onrejected === undefined || onrejected === null) throw reason;
        return Promise.resolve(onrejected(reason));
      })
    );
  }

  catch<TResult = never>(
    onrejected?: ((reason: unknown) => TResult | PromiseLike<TResult>) | null
  ): Promise<T | TResult> {
    return this.then(undefined, onrejected);
  }

  finally(onfinally?: (() => void) | null): Promise<T> {
    return this.then(
      (value) => this.serial.execute(() => { onfinally?.(); return value; }),
      (reason) => this.serial.execute(() => { onfinally?.(); throw reason; })
    );
  }
}

function workerQueueFull(maxQueueLength: number): ZLinkFrameworkException {
  return createInternalFrameworkException(
    ZLinkFrameworkInternalErrorKind.WorkerQueueFull,
    `CPU worker queue is full (maxQueueLength=${maxQueueLength}).`,
    true
  );
}

function workerTimedOut(timeoutMs: number): ZLinkFrameworkException {
  return createInternalFrameworkException(
    ZLinkFrameworkInternalErrorKind.WorkerTimedOut,
    `Worker job timed out after ${timeoutMs}ms.`,
    true
  );
}

function workerFailed(cause: unknown): ZLinkFrameworkException {
  return createInternalFrameworkException(
    ZLinkFrameworkInternalErrorKind.WorkerFailed,
    'Worker job failed.',
    false,
    cause
  );
}

function requireNonNegativeInteger(label: string, value: number): void {
  if (!Number.isSafeInteger(value) || value < 0) {
    throw new ZLinkConfigurationException(`${label} must be a non-negative safe integer.`);
  }
}

function requirePositiveInteger(label: string, value: number): void {
  if (!Number.isSafeInteger(value) || value <= 0) {
    throw new ZLinkConfigurationException(`${label} must be a positive safe integer.`);
  }
}

function deserializeWorkerError(error: CpuWorkerMessage['error']): Error {
  const result = new Error(error?.message ?? 'CPU worker failed.');
  result.name = error?.name ?? 'Error';
  if (error?.stack !== undefined) result.stack = error.stack;
  return result;
}
