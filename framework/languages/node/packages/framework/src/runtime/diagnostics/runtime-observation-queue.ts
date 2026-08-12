import type {
  ZLinkObservationLoss,
  ZLinkObservedStatus
} from '../../contracts';

export const ZLINK_DEFAULT_TERMINAL_OBSERVATION_CAPACITY = 64;
export const ZLINK_SIGNED_OBSERVATION_LOSS_MAXIMUM = 9_223_372_036_854_775_807n;

const DEFAULT_OBSERVATION_SOURCE = Symbol('zlink.runtime-observation.default-source');

interface RetainedObservation<T> {
  readonly source: unknown;
  readonly status: T;
  readonly publishOrdinal: bigint;
}

interface RuntimeObservationDispatchTarget {
  dispatchRetainedObservation(): void;
}

/** One process-wide event-loop dispatcher serves every observation subscriber. */
class RuntimeObservationDispatcher {
  private readonly ready = new Set<RuntimeObservationDispatchTarget>();
  private scheduled = false;

  schedule(target: RuntimeObservationDispatchTarget): void {
    this.ready.add(target);
    this.requestFlush();
  }

  private requestFlush(): void {
    if (this.scheduled) return;
    this.scheduled = true;
    queueMicrotask(() => this.flush());
  }

  private flush(): void {
    this.scheduled = false;
    const ready = [...this.ready];
    this.ready.clear();
    for (const target of ready) target.dispatchRetainedObservation();
    if (this.ready.size !== 0) this.requestFlush();
  }
}

const runtimeObservationDispatcher = new RuntimeObservationDispatcher();

/** Owns the two independent signed-64 saturating subscriber loss counters. */
class RuntimeObservationLossState {
  private coalesced = 0n;
  private discardedTerminal = 0n;

  recordCoalesced(): void {
    this.coalesced = saturatingObservationLossIncrement(this.coalesced);
  }

  recordDiscardedTerminal(): void {
    this.discardedTerminal = saturatingObservationLossIncrement(
      this.discardedTerminal
    );
  }

  snapshot(): ZLinkObservationLoss {
    return {
      coalescedCount: this.coalesced,
      discardedTerminalCount: this.discardedTerminal
    };
  }
}

/**
 * Retains source-latest intermediate states and a bounded terminal FIFO for
 * one subscriber. Producers only mutate retained state and signal the shared
 * dispatcher; they never invoke an iterator continuation directly.
 */
export class RuntimeEventQueue<T>
  implements AsyncIterable<ZLinkObservedStatus<T>>, AsyncIterator<ZLinkObservedStatus<T>>,
    RuntimeObservationDispatchTarget {
  private readonly intermediateBySource = new Map<unknown, RetainedObservation<T>>();
  private readonly terminalFifo: Array<RetainedObservation<T> | undefined> = [];
  private readonly terminalCountBySource = new Map<unknown, number>();
  private terminalHead = 0;
  private terminalCount = 0;
  private readonly waiters: Array<
    ((result: IteratorResult<ZLinkObservedStatus<T>>) => void) | undefined
  > = [];
  private waitersHead = 0;
  private waitersCount = 0;
  private cleanup?: () => void;
  private abortCleanup?: () => void;
  private readonly loss = new RuntimeObservationLossState();
  private nextPublishOrdinal = 1n;
  private sealed = false;
  private closed = false;
  private completeWhenDrained = false;

  constructor(
    private readonly terminalCapacity = ZLINK_DEFAULT_TERMINAL_OBSERVATION_CAPACITY,
    signal?: AbortSignal
  ) {
    if (!Number.isInteger(terminalCapacity) || terminalCapacity <= 0) {
      throw new RangeError('Observer capacity must be positive.');
    }
    if (signal !== undefined) {
      const onAbort = () => this.close();
      signal.addEventListener('abort', onAbort, { once: true });
      this.abortCleanup = () => signal.removeEventListener('abort', onAbort);
    }
  }

  [Symbol.asyncIterator](): AsyncIterator<ZLinkObservedStatus<T>> { return this; }

  next(): Promise<IteratorResult<ZLinkObservedStatus<T>>> {
    if (this.waitersCount === 0) {
      const retained = this.takeRetained();
      if (retained !== undefined) {
        const result = Promise.resolve({
          done: false as const,
          value: this.observed(retained.status)
        });
        this.finishIfDrained();
        return result;
      }
      if (this.closed || this.completeWhenDrained) {
        this.finishClosed();
        return Promise.resolve({ done: true, value: undefined });
      }
    }
    return new Promise(resolve => {
      this.waiters.push(resolve);
      this.waitersCount += 1;
      if (this.hasRetained() || this.closed || this.completeWhenDrained) {
        runtimeObservationDispatcher.schedule(this);
      }
    });
  }

  return(): Promise<IteratorResult<ZLinkObservedStatus<T>>> {
    this.close();
    return Promise.resolve({ done: true, value: undefined });
  }

  onClose(cleanup: () => void): void {
    if (this.closed) cleanup();
    else this.cleanup = cleanup;
  }

  push(value: T, source: unknown = DEFAULT_OBSERVATION_SOURCE): void {
    if (this.closed || this.sealed) return;
    if ((this.terminalCountBySource.get(source) ?? 0) !== 0) {
      this.loss.recordCoalesced();
      return;
    }
    if (this.intermediateBySource.has(source)) this.loss.recordCoalesced();
    this.intermediateBySource.set(source, this.retain(value, source));
    this.signalDispatcher();
  }

  pushTerminal(value: T, source: unknown = DEFAULT_OBSERVATION_SOURCE): void {
    if (this.closed || this.sealed) return;
    this.retainTerminal(value, source);
  }

  seal(value: T, source: unknown = DEFAULT_OBSERVATION_SOURCE): void {
    if (this.closed) return;
    if (!this.sealed) {
      this.sealed = true;
      this.detachSource();
    }
    this.retainTerminal(value, source);
  }

  complete(value: T, source: unknown = DEFAULT_OBSERVATION_SOURCE): void {
    if (this.closed) return;
    if (!this.sealed) {
      this.sealed = true;
      this.detachSource();
    }
    this.completeWhenDrained = true;
    this.retainTerminal(value, source);
  }

  close(): void {
    if (this.closed) return;
    this.closed = true;
    this.completeWhenDrained = false;
    this.intermediateBySource.clear();
    this.terminalFifo.length = 0;
    this.terminalHead = 0;
    this.terminalCount = 0;
    this.terminalCountBySource.clear();
    this.detachAbort();
    this.detachSource();
    if (this.waitersCount !== 0) runtimeObservationDispatcher.schedule(this);
  }

  dispatchRetainedObservation(): void {
    let waiter: ((result: IteratorResult<ZLinkObservedStatus<T>>) => void) | undefined;
    while (this.waitersCount !== 0 && this.hasRetained()) {
      waiter = this.takeWaiter();
      const retained = this.takeRetained();
      if (waiter === undefined || retained === undefined) break;
      waiter({ done: false, value: this.observed(retained.status) });
    }
    this.finishIfDrained();
    if (!this.closed) return;
    while ((waiter = this.takeWaiter()) !== undefined) {
      waiter({ done: true, value: undefined });
    }
  }

  private retainTerminal(value: T, source: unknown): void {
    if (this.intermediateBySource.delete(source)) this.loss.recordCoalesced();
    if (this.terminalCount === this.terminalCapacity) {
      const discarded = this.takeTerminal();
      if (discarded !== undefined) this.loss.recordDiscardedTerminal();
    }
    this.terminalFifo.push(this.retain(value, source));
    this.terminalCount += 1;
    this.terminalCountBySource.set(
      source,
      (this.terminalCountBySource.get(source) ?? 0) + 1
    );
    this.signalDispatcher();
  }

  private signalDispatcher(): void {
    if (this.waitersCount !== 0) runtimeObservationDispatcher.schedule(this);
  }

  private hasRetained(): boolean {
    return this.terminalCount !== 0 || this.intermediateBySource.size !== 0;
  }

  private takeRetained(): RetainedObservation<T> | undefined {
    const terminal = this.peekTerminal();
    const intermediate = this.oldestIntermediate();
    if (
      terminal !== undefined
      && (intermediate === undefined
        || terminal.publishOrdinal <= intermediate.publishOrdinal)
    ) {
      return this.takeTerminal();
    }
    if (intermediate === undefined) return undefined;
    this.intermediateBySource.delete(intermediate.source);
    return intermediate;
  }

  private peekTerminal(): RetainedObservation<T> | undefined {
    return this.terminalCount === 0
      ? undefined
      : this.terminalFifo[this.terminalHead];
  }

  private oldestIntermediate(): RetainedObservation<T> | undefined {
    let oldest: RetainedObservation<T> | undefined;
    for (const candidate of this.intermediateBySource.values()) {
      if (oldest === undefined || candidate.publishOrdinal < oldest.publishOrdinal) {
        oldest = candidate;
      }
    }
    return oldest;
  }

  private takeTerminal(): RetainedObservation<T> | undefined {
    if (this.terminalCount === 0) return undefined;
    const retained = this.terminalFifo[this.terminalHead];
    this.terminalFifo[this.terminalHead] = undefined;
    this.terminalHead += 1;
    this.terminalCount -= 1;
    if (retained !== undefined) this.releaseTerminalSource(retained.source);
    this.compactTerminalFifo();
    return retained;
  }

  private releaseTerminalSource(source: unknown): void {
    const count = this.terminalCountBySource.get(source);
    if (count === undefined || count <= 1) {
      this.terminalCountBySource.delete(source);
    } else {
      this.terminalCountBySource.set(source, count - 1);
    }
  }

  private retain(status: T, source: unknown): RetainedObservation<T> {
    const retained = {
      source,
      status,
      publishOrdinal: this.nextPublishOrdinal
    };
    this.nextPublishOrdinal += 1n;
    return retained;
  }

  private compactTerminalFifo(): void {
    if (this.terminalCount === 0) {
      this.terminalFifo.length = 0;
      this.terminalHead = 0;
    } else if (
      this.terminalHead >= 1024
      && this.terminalHead * 2 >= this.terminalFifo.length
    ) {
      this.terminalFifo.splice(0, this.terminalHead);
      this.terminalHead = 0;
    }
  }

  private finishIfDrained(): void {
    if (this.completeWhenDrained && !this.hasRetained()) this.finishClosed();
  }

  private finishClosed(): void {
    if (this.closed) return;
    this.closed = true;
    this.completeWhenDrained = false;
    this.detachAbort();
    this.detachSource();
    if (this.waitersCount !== 0) runtimeObservationDispatcher.schedule(this);
  }

  private detachSource(): void {
    this.cleanup?.();
    this.cleanup = undefined;
  }

  private detachAbort(): void {
    this.abortCleanup?.();
    this.abortCleanup = undefined;
  }

  private observed(status: T): ZLinkObservedStatus<T> {
    return { status, loss: this.loss.snapshot() };
  }

  private takeWaiter():
    | ((result: IteratorResult<ZLinkObservedStatus<T>>) => void)
    | undefined {
    if (this.waitersCount === 0) return undefined;
    const waiter = this.waiters[this.waitersHead];
    this.waiters[this.waitersHead] = undefined;
    this.waitersHead += 1;
    this.waitersCount -= 1;
    if (this.waitersCount === 0) {
      this.waiters.length = 0;
      this.waitersHead = 0;
    } else if (this.waitersHead >= 1024 && this.waitersHead * 2 >= this.waiters.length) {
      this.waiters.splice(0, this.waitersHead);
      this.waitersHead = 0;
    }
    return waiter;
  }
}

export function saturatingObservationLossIncrement(value: bigint): bigint {
  if (value < 0n || value > ZLINK_SIGNED_OBSERVATION_LOSS_MAXIMUM) {
    throw new RangeError('Observation loss must fit a non-negative signed 64-bit integer.');
  }
  return value >= ZLINK_SIGNED_OBSERVATION_LOSS_MAXIMUM
    ? ZLINK_SIGNED_OBSERVATION_LOSS_MAXIMUM
    : value + 1n;
}
