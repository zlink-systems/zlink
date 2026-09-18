import {
  Disposable,
  ZlinkStreamEncodedPayload,
  ZlinkStreamErrorCode,
  ZlinkStreamMessage
} from '../Contracts';
import { validateName } from './Protocol/ZlinkStreamPacketNameValidator';
import type { ZlinkStreamConnectorEvents } from './ZlinkStreamConnectorEvents';
import { subscription } from './ZlinkStreamSupport';

type EncodedMessageHandler = (
  message: ZlinkStreamMessage<ZlinkStreamEncodedPayload>,
  signal?: AbortSignal
) => Promise<void> | void;

/**
 * A wait surface (`waitFor`, `expectNone`, `waitForSequence`) reading the queue
 * directly. It returns true when it consumed the message and must not throw:
 * a failed predicate is reported through the waiting call, not to the queue.
 */
type EncodedMessageObserver = (
  message: ZlinkStreamMessage<ZlinkStreamEncodedPayload>
) => boolean;

interface QueuedMessage {
  readonly message: ZlinkStreamMessage<ZlinkStreamEncodedPayload>;
  readonly signal?: AbortSignal;
}

export class ZlinkStreamReceivedMessages {
  private readonly handlers = new Map<string, Set<EncodedMessageHandler>>();
  private readonly observers = new Map<string, Set<EncodedMessageObserver>>();
  // A handler can be registered after messages for another name arrive, so the
  // queue is not a simple FIFO. Tombstones let us remove a deliverable entry
  // without shifting every later message on the hot receive path.
  private readonly queue: Array<QueuedMessage | undefined> = [];
  private queueHead = 0;
  private queuedCount = 0;
  private drainTask: Promise<void> | undefined;
  // True for as long as `drain` is on the stack, handler awaits included. It
  // marks the execution context a registered handler runs in, so a `dispatch`
  // made from inside a handler is recognised as re-entry rather than a fresh
  // pump. It is not a lock: a single event loop admits no second thread, and
  // nothing ever waits for this flag to fall.
  private draining = false;
  // Spec stream-connector 32 §10: arrivals per packet name on the current
  // connection. It is raised where a packet arrives, never where one is taken,
  // so consuming does not lower it and the dispatch mode does not change it.
  private readonly receivedCounts = new Map<string, number>();

  /**
   * @param deliverOnArrival `Immediate` runs registered handlers on the receive
   *   path; `Manual` leaves them queued until {@link pump} runs them on the
   *   caller's thread (spec stream-connector 32 §7). Wait surfaces observe the
   *   queue in both modes, so they never depend on this flag.
   */
  constructor(
    private readonly events: ZlinkStreamConnectorEvents,
    private readonly deliverOnArrival: boolean
  ) {}

  on(name: string, handler: EncodedMessageHandler): Disposable {
    validateName(name);
    let set = this.handlers.get(name);
    if (set === undefined) {
      set = new Set();
      this.handlers.set(name, set);
    }
    set.add(handler);
    if (this.deliverOnArrival && this.hasQueuedMessage(name)) {
      queueMicrotask(() => this.scheduleDrain());
    }
    return subscription(() => {
      set.delete(handler);
      if (set.size === 0 && this.handlers.get(name) === set) {
        this.handlers.delete(name);
      }
    });
  }

  /**
   * Registers a wait surface over the receive queue. Spec stream-connector 32
   * §7: these are not registered callbacks — they observe and consume the
   * packets the queue has not delivered yet, in both dispatch modes, so
   * `Manual` needs no dispatch pump to complete a wait. The queue is scanned in
   * a microtask so a message that arrived before the wait started is still
   * observed, and so the caller has its subscription in hand by then.
   */
  observe(name: string, observer: EncodedMessageObserver): Disposable {
    validateName(name);
    let set = this.observers.get(name);
    if (set === undefined) {
      set = new Set();
      this.observers.set(name, set);
    }
    set.add(observer);
    queueMicrotask(() => {
      if (this.observers.get(name)?.has(observer) === true) {
        this.offerQueued(name, observer);
      }
    });
    return subscription(() => {
      set.delete(observer);
      if (set.size === 0 && this.observers.get(name) === set) {
        this.observers.delete(name);
      }
    });
  }

  /** Spec stream-connector 32 §10: arrivals under `name` on this connection. */
  receivedCount(name: string): number {
    return this.receivedCounts.get(name) ?? 0;
  }

  /** A newly established connection counts from 0 again (spec §10). */
  resetReceivedCounts(): void {
    this.receivedCounts.clear();
  }

  enqueue(message: ZlinkStreamMessage<ZlinkStreamEncodedPayload>, signal?: AbortSignal): void {
    this.receivedCounts.set(message.name, (this.receivedCounts.get(message.name) ?? 0) + 1);
    for (const observer of [...this.observers.get(message.name) ?? []]) {
      if (observer(message)) {
        return;
      }
    }
    this.queue.push({ message, signal });
    this.queuedCount += 1;
    if (this.deliverOnArrival) {
      this.scheduleDrain();
    }
  }

  /**
   * Runs the registered handlers the receive path left queued. `Manual` calls
   * this from `dispatch`; `Immediate` has already drained on arrival.
   *
   * A handler that calls `dispatch` arrives back here from inside the drain it
   * was started by. `scheduleDrain` would find `drainTask` already set and
   * return, and the await below would then be the drain waiting on itself —
   * a deadlock with neither timeout nor error. The drain loop already takes
   * every message a handler exists for, so there is nothing a second drain
   * would deliver and returning is the whole of the correct behaviour.
   */
  async pump(): Promise<void> {
    if (this.draining) {
      return;
    }
    this.scheduleDrain();
    await this.drainTask;
  }

  private offerQueued(name: string, observer: EncodedMessageObserver): void {
    for (let index = this.queueHead; index < this.queue.length; index += 1) {
      const queued = this.queue[index];
      if (queued === undefined || queued.message.name !== name) {
        continue;
      }
      if (!observer(queued.message)) {
        continue;
      }
      this.removeAt(index);
      return;
    }
  }

  private scheduleDrain(): void {
    if (this.drainTask !== undefined) {
      return;
    }
    this.drainTask = this.drain().finally(() => {
      this.drainTask = undefined;
      if (this.deliverOnArrival && this.findDeliverableIndex() >= 0) {
        this.scheduleDrain();
      }
    });
  }

  private async drain(): Promise<void> {
    this.draining = true;
    try {
      for (let index = this.findDeliverableIndex(); index >= 0; index = this.findDeliverableIndex()) {
        const queued = this.queue[index];
        if (queued === undefined) continue;
        this.removeAt(index);
        const { message, signal } = queued;
        const handlers = [...this.handlers.get(message.name)!];
        for (const handler of handlers) {
          try {
            await handler(message, signal);
          } catch (cause) {
            await this.events.publishError({
              code: ZlinkStreamErrorCode.UserCallbackFailed,
              message: 'Typed message handler failed.',
              cause
            }, signal);
          }
        }
      }
    } finally {
      this.draining = false;
    }
  }

  private removeAt(index: number): void {
    this.queue[index] = undefined;
    this.queuedCount -= 1;
    this.advanceHead();
    this.compactQueue();
  }

  private findDeliverableIndex(): number {
    for (let index = this.queueHead; index < this.queue.length; index += 1) {
      const queued = this.queue[index];
      if (queued !== undefined && (this.handlers.get(queued.message.name)?.size ?? 0) > 0) {
        return index;
      }
    }
    return -1;
  }

  private hasQueuedMessage(name: string): boolean {
    for (let index = this.queueHead; index < this.queue.length; index += 1) {
      if (this.queue[index]?.message.name === name) return true;
    }
    return false;
  }

  private advanceHead(): void {
    while (this.queueHead < this.queue.length && this.queue[this.queueHead] === undefined) {
      this.queueHead += 1;
    }
  }

  private compactQueue(): void {
    if (this.queuedCount === 0) {
      this.queue.length = 0;
      this.queueHead = 0;
      return;
    }
    if (this.queueHead >= 1024 && this.queueHead * 2 >= this.queue.length) {
      this.queue.splice(0, this.queueHead);
      this.queueHead = 0;
    }
  }
}
