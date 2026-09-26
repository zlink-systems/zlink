import { Disposable, ZlinkStreamEncodedPayload, ZlinkStreamMessage } from '../Contracts';
import { validateName } from './Protocol/ZlinkStreamPacketNameValidator';
import type { ZlinkStreamConnectorEvents } from './ZlinkStreamConnectorEvents';
import { subscription } from './ZlinkStreamSupport';
import { zlinkStreamActorBinding } from './ZlinkStreamActors';

type EncodedMessageHandler = (
  message: ZlinkStreamMessage<ZlinkStreamEncodedPayload>,
  signal?: AbortSignal
) => Promise<void> | void;

/**
 * A wait surface (`waitFor`, `expectNone`, `waitForSequence`) reading the queue
 * directly. It returns true when it consumed the message and must not throw:
 * a failed predicate is reported through the waiting call, not to the queue.
 */
type EncodedMessageObserver = (message: ZlinkStreamMessage<ZlinkStreamEncodedPayload>) => boolean;

interface QueuedMessage {
  readonly kind: 'message';
  readonly message: ZlinkStreamMessage<ZlinkStreamEncodedPayload>;
  readonly signal?: AbortSignal;
  indexed?: boolean;
}

interface QueuedCallback {
  readonly kind: 'callback';
  readonly callback: () => Promise<void> | void;
  /** The handler calls `callback` makes if the pump ran it now. */
  readonly callbacks: () => number;
  indexed?: boolean;
}

type QueuedDispatch = QueuedMessage | QueuedCallback;

/**
 * A registered push handler. `actor` is set for an Actor handle's handler,
 * which receives only the packets carrying that Actor's slot.
 */
interface RegisteredHandler {
  readonly handle: EncodedMessageHandler;
  readonly actor?: object;
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

function receives(
  registration: RegisteredHandler,
  message: ZlinkStreamMessage<ZlinkStreamEncodedPayload>
): boolean {
  return (
    registration.actor === undefined ||
    registration.actor ===
      (message as { [zlinkStreamActorBinding]?: object })[zlinkStreamActorBinding]
  );
}

export class ZlinkStreamReceivedMessages {
  private readonly handlers = new Map<string, Set<RegisteredHandler>>();
  private readonly observers = new Map<string, Set<RegisteredObserver>>();
  // A handler can be registered after messages for another name arrive, so the
  // queue is not a simple FIFO. Tombstones let us remove a deliverable entry
  // without shifting every later message on the hot receive path.
  private readonly queue: Array<QueuedDispatch | undefined> = [];
  private readonly deliverable: number[] = [];
  private queueHead = 0;
  private queuedCount = 0;
  // True for as long as `pump` is on the stack. It
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

  /**
   * @param actor The Actor handle that registers the handler, if any. Its
   *   handler receives only that Actor's packets.
   */
  on(name: string, handler: EncodedMessageHandler, actor?: object): Disposable {
    validateName(name);
    let set = this.handlers.get(name);
    if (set === undefined) {
      set = new Set();
      this.handlers.set(name, set);
    }
    const registration: RegisteredHandler = { handle: handler, actor };
    set.add(registration);
    let newlyDeliverable = false;
    for (let index = this.queueHead; index < this.queue.length; index += 1) {
      const queued = this.queue[index];
      if (
        queued?.kind === 'message' &&
        queued.indexed !== true &&
        queued.message.name === name &&
        receives(registration, queued.message)
      ) {
        this.indexDeliverable(index);
        newlyDeliverable = true;
      }
    }
    if (this.deliverOnArrival && newlyDeliverable) {
      queueMicrotask(() => this.pump());
    }
    return subscription(() => {
      set.delete(registration);
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
    // Messages belong to the connection that received them, but callbacks are
    // already-submitted lifecycle work. In Manual mode a reconnect can finish
    // before the application next pumps dispatch; keep those callbacks so the
    // old connection's unbound/state/disconnected sequence remains observable.
    const submittedCallbacks = this.queue
      .slice(this.queueHead)
      .filter((item): item is QueuedCallback => item?.kind === 'callback');
    this.queue.length = 0;
    this.queue.push(...submittedCallbacks);
    this.queueHead = 0;
    this.queuedCount = submittedCallbacks.length;
    this.deliverable.length = 0;
    for (let index = 0; index < this.queue.length; index += 1) {
      this.queue[index]!.indexed = false;
      this.indexDeliverable(index);
    }
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
    const abandoned = Array.from(this.observers.values()).flatMap((set) => Array.from(set));
    this.observers.clear();
    for (const registration of abandoned) {
      registration.onConnectionEnded();
    }
  }

  enqueue(message: ZlinkStreamMessage<ZlinkStreamEncodedPayload>, signal?: AbortSignal): void {
    this.receivedCounts.set(message.name, (this.receivedCounts.get(message.name) ?? 0) + 1);
    for (const registration of Array.from(this.observers.get(message.name) ?? [])) {
      if (registration.consume(message)) {
        return;
      }
    }
    this.queue.push({ kind: 'message', message, signal });
    this.queuedCount += 1;
    this.indexDeliverable(this.queue.length - 1);
    if (this.deliverOnArrival) {
      this.pump();
    }
  }

  /**
   * @param callbacks The number of handler calls `callback` makes when it runs
   *   with the handlers registered at that moment; a callback that runs a set
   *   of handlers reads the set's size. One callback by default.
   */
  enqueueCallback(callback: () => Promise<void> | void, callbacks: () => number = () => 1): void {
    this.queue.push({ kind: 'callback', callback, callbacks });
    this.queuedCount += 1;
    this.indexDeliverable(this.queue.length - 1);
    if (this.deliverOnArrival) {
      this.pump();
    }
  }

  /**
   * Runs the registered handlers the receive path left queued. `Manual` calls
   * this from `dispatch`; `Immediate` drains on arrival.
   *
   * The drain is synchronous: the connector does not wait for a handler
   * (spec stream-connector 32 §7), so nothing inside it awaits. A handler that
   * calls `dispatch` arrives back here from inside the drain it was started by;
   * the running loop already takes every deliverable entry, so returning is the
   * whole of the correct behaviour.
   */
  pump(): void {
    if (this.draining) {
      return;
    }
    this.draining = true;
    try {
      for (
        let index = this.findDeliverableIndex();
        index >= 0;
        index = this.findDeliverableIndex()
      ) {
        const queued = this.queue[index];
        if (queued === undefined) continue;
        this.removeAt(index);
        if (queued.kind === 'callback') {
          this.events.runUserCallback(queued.callback, 'Connector callback failed.');
          continue;
        }
        const { message, signal } = queued;
        const handlers = this.receiversOf(message);
        for (const handler of handlers) {
          this.events.runUserCallback(
            () => handler.handle(message, signal),
            'Typed message handler failed.',
            signal
          );
        }
      }
    } finally {
      this.draining = false;
    }
  }

  /**
   * True while a registered callback runs from {@link pump}: the execution
   * context spec stream-connector 32 §7 calls "inside a handler".
   */
  get dispatching(): boolean {
    return this.draining;
  }

  get pendingCallbacks(): number {
    let count = 0;
    for (let index = this.queueHead; index < this.queue.length; index += 1) {
      const queued = this.queue[index];
      if (queued?.kind === 'callback' && queued.callbacks() > 0) count += 1;
      if (queued?.kind === 'message' && this.receiversOf(queued.message).length > 0) count += 1;
    }
    return count;
  }

  private offerQueued(name: string, registration: RegisteredObserver): void {
    for (let index = this.queueHead; index < this.queue.length; index += 1) {
      const queued = this.queue[index];
      if (queued === undefined || queued.kind !== 'message' || queued.message.name !== name) {
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
      for (
        let index = this.findDeliverableIndex();
        index >= 0;
        index = this.findDeliverableIndex()
      ) {
        const queued = this.queue[index];
        if (queued === undefined) continue;
        this.removeAt(index);
        if (queued.kind === 'callback') {
          await queued.callback();
          continue;
        }
        // Not `const { message, signal } = queued`: emscripten 3.1.38's JSDCE
        // (bundled into the Unity WebGL browser build, see
        // test/browser/unity-webgl-emscripten.test.js) reads a destructuring
        // declarator's `node.id.name`, which is undefined for a pattern, and
        // deletes any declarator whose id.name reads as the unreferenced
        // identifier `undefined`. Plain property reads carry a real `id.name`
        // and are not affected.
        const message = queued.message;
        const signal = queued.signal;
        const handlers = Array.from(this.handlers.get(message.name)!);
        for (const handler of handlers) {
          try {
            await handler(message, signal);
          } catch (cause) {
            await this.events.publishError(
              {
                code: ZlinkStreamErrorCode.UserCallbackFailed,
                message: 'Typed message handler failed.',
                cause
              },
              signal
            );
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
    while (this.deliverable.length > 0) {
      const index = this.deliverable[0];
      const queued = this.queue[index];
      if (this.isDeliverable(queued)) {
        return index;
      }
      if (queued !== undefined) queued.indexed = false;
      this.removeDeliverable();
    }
    return -1;
  }

  private indexDeliverable(index: number): void {
    const queued = this.queue[index];
    if (queued === undefined || !this.isDeliverable(queued) || queued.indexed === true) return;
    queued.indexed = true;
    let position = this.deliverable.length;
    this.deliverable.push(index);
    while (position > 0) {
      const parent = Math.floor((position - 1) / 2);
      if (this.deliverable[parent] <= index) break;
      this.deliverable[position] = this.deliverable[parent];
      position = parent;
    }
    this.deliverable[position] = index;
  }

  private isDeliverable(queued: QueuedDispatch | undefined): boolean {
    return (
      queued?.kind === 'callback' ||
      (queued?.kind === 'message' && this.receiversOf(queued.message).length > 0)
    );
  }

  /**
   * Spec stream-connector 32 §7 and §10: the handlers that receive `message`
   * are the connector handlers for its name and the handle handlers of the
   * Actor it carries. A packet no handler receives stays in the queue for the
   * wait surfaces. This is the one place that decides it.
   */
  private receiversOf(message: ZlinkStreamMessage<ZlinkStreamEncodedPayload>): RegisteredHandler[] {
    const set = this.handlers.get(message.name);
    if (set === undefined) return [];
    return Array.from(set).filter((registration) => receives(registration, message));
  }

  private removeDeliverable(): void {
    if (this.deliverable.length === 0) return;
    const last = this.deliverable.pop()!;
    if (this.deliverable.length === 0) return;
    let position = 0;
    for (;;) {
      const left = position * 2 + 1;
      if (left >= this.deliverable.length) break;
      const right = left + 1;
      const child =
        right < this.deliverable.length && this.deliverable[right] < this.deliverable[left]
          ? right
          : left;
      if (this.deliverable[child] >= last) break;
      this.deliverable[position] = this.deliverable[child];
      position = child;
    }
    this.deliverable[position] = last;
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
      this.deliverable.length = 0;
      return;
    }
    if (this.queueHead >= 1024 && this.queueHead * 2 >= this.queue.length) {
      this.queue.splice(0, this.queueHead);
      this.queueHead = 0;
      this.deliverable.length = 0;
      for (let index = 0; index < this.queue.length; index += 1) {
        const queued = this.queue[index];
        if (queued === undefined) continue;
        queued.indexed = false;
        this.indexDeliverable(index);
      }
    }
  }
}
