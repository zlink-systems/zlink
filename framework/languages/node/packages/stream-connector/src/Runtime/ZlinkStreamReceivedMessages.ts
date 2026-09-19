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

/**
 * A registered wait surface plus what to call when the connection it is
 * watching ends before its predicate matched. `onConnectionEnded` is how
 * {@link ZlinkStreamReceivedMessages.connectionEnded} fails a wait the moment
 * its connection ends (spec stream-connector 32 §10.1.1, Java
 * `ZLinkStreamDispatchQueue.connectionEnded` parity).
 */
interface RegisteredObserver {
  readonly consume: EncodedMessageObserver;
  readonly onConnectionEnded: () => void;
}

export class ZlinkStreamReceivedMessages {
  private readonly handlers = new Map<string, Set<EncodedMessageHandler>>();
  private readonly observers = new Map<string, Set<RegisteredObserver>>();
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
   *
   * @param onConnectionEnded Called, instead of {@link observer}, when
   *   {@link connectionEnded} abandons this registration because the
   *   connection it was watching ended before a message matched (spec
   *   stream-connector 32 §10.1.1: "연결이 끝나 대기를 이어갈 수 없으면
   *   `Disconnected`다", released when that connection ends).
   */
  observe(
    name: string,
    observer: EncodedMessageObserver,
    onConnectionEnded: () => void
  ): Disposable {
    validateName(name);
    let set = this.observers.get(name);
    if (set === undefined) {
      set = new Set();
      this.observers.set(name, set);
    }
    const registration: RegisteredObserver = { consume: observer, onConnectionEnded };
    set.add(registration);
    queueMicrotask(() => {
      if (this.observers.get(name)?.has(registration) === true) {
        this.offerQueued(name, registration);
      }
    });
    return subscription(() => {
      set.delete(registration);
      if (set.size === 0 && this.observers.get(name) === set) {
        this.observers.delete(name);
      }
    });
  }

  /** Spec stream-connector 32 §10: arrivals under `name` on this connection. */
  receivedCount(name: string): number {
    return this.receivedCounts.get(name) ?? 0;
  }

  /**
   * Rebaselines the queue for a connection that was just established. Spec
   * stream-connector 32 §10 (line ~649): the reference point is the moment a
   * connection is established, so counts restart at 0 and whatever the
   * previous connection left unconsumed goes with it — keeping the queue
   * while only the counts reset would let counts and queue describe two
   * different connections, and let `waitFor` hand back a packet from before
   * the drop as if the new connection had delivered it.
   *
   * `ZlinkStreamMessage`/`ZlinkStreamEncodedPayload` are plain data (name,
   * metadata, a `Uint8Array` payload) with no dispose/close of their own —
   * unlike Java's queued frames, which `closeMessage` releases — so dropping
   * the queue's references is the whole of the release here.
   *
   * Wait surfaces are not touched here. The ones of the previous connection
   * were released by {@link connectionEnded} when that connection ended, and
   * one registered since then is waiting for this connection.
   */
  resetForNewConnection(): void {
    this.receivedCounts.clear();
    this.queue.length = 0;
    this.queueHead = 0;
    this.queuedCount = 0;
  }

  /**
   * Releases every registered wait surface because the connection it was
   * watching has ended — a transport loss, a server close, or `close()`.
   * Spec stream-connector 32 §10.1.1: "푸는 시점은 연결이 끝난 때이지 다음
   * 연결이 성립한 때가 아니다". The release belongs to the ending, so a wait
   * does not hang until its own timeout when no next connection comes
   * (reconnect off, attempts spent) and does not silently rebind to the next
   * one when it does. The queue and the counts stay: they are rebaselined by
   * the next {@link resetForNewConnection}, not by the ending (§10).
   */
  connectionEnded(): void {
    if (this.observers.size === 0) {
      return;
    }
    const abandoned = [...this.observers.values()].flatMap((set) => [...set]);
    this.observers.clear();
    for (const registration of abandoned) {
      registration.onConnectionEnded();
    }
  }

  enqueue(message: ZlinkStreamMessage<ZlinkStreamEncodedPayload>, signal?: AbortSignal): void {
    this.receivedCounts.set(message.name, (this.receivedCounts.get(message.name) ?? 0) + 1);
    for (const registration of [...this.observers.get(message.name) ?? []]) {
      if (registration.consume(message)) {
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

  private offerQueued(name: string, registration: RegisteredObserver): void {
    for (let index = this.queueHead; index < this.queue.length; index += 1) {
      const queued = this.queue[index];
      if (queued === undefined || queued.message.name !== name) {
        continue;
      }
      if (!registration.consume(queued.message)) {
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
