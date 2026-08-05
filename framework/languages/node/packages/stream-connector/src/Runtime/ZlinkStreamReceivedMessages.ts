import {
  Disposable,
  ZlinkStreamEncodedPayload,
  ZlinkStreamErrorCode,
  ZlinkStreamMessage
} from '../Contracts';
import { validateName } from './Protocol/ZlinkStreamPacketNameValidator';
import { subscription } from './ZlinkStreamSupport';
import type { ZlinkStreamConnectorEvents } from './ZlinkStreamConnectorEvents';

type EncodedMessageHandler = (
  message: ZlinkStreamMessage<ZlinkStreamEncodedPayload>,
  signal?: AbortSignal
) => Promise<void> | void;

interface QueuedMessage {
  readonly message: ZlinkStreamMessage<ZlinkStreamEncodedPayload>;
  readonly signal?: AbortSignal;
}

export class ZlinkStreamReceivedMessages {
  private readonly handlers = new Map<string, Set<EncodedMessageHandler>>();
  // A handler can be registered after messages for another name arrive, so the
  // queue is not a simple FIFO. Tombstones let us remove a deliverable entry
  // without shifting every later message on the hot receive path.
  private readonly queue: Array<QueuedMessage | undefined> = [];
  private queueHead = 0;
  private queuedCount = 0;
  private drainTask: Promise<void> | undefined;
  private dropReportPending = false;

  constructor(
    private readonly capacity: number,
    private readonly events: ZlinkStreamConnectorEvents
  ) {}

  on(name: string, handler: EncodedMessageHandler): Disposable {
    validateName(name);
    let set = this.handlers.get(name);
    if (set === undefined) {
      set = new Set();
      this.handlers.set(name, set);
    }
    set.add(handler);
    if (this.hasQueuedMessage(name)) {
      queueMicrotask(() => this.scheduleDrain());
    }
    return subscription(() => {
      set.delete(handler);
      if (set.size === 0 && this.handlers.get(name) === set) {
        this.handlers.delete(name);
      }
    });
  }

  enqueue(message: ZlinkStreamMessage<ZlinkStreamEncodedPayload>, signal?: AbortSignal): void {
    if (this.queuedCount >= this.capacity) {
      this.reportDropped(signal);
      return;
    }
    this.queue.push({ message, signal });
    this.queuedCount += 1;
    this.scheduleDrain();
  }

  private scheduleDrain(): void {
    if (this.drainTask !== undefined) {
      return;
    }
    this.drainTask = this.drain().finally(() => {
      this.drainTask = undefined;
      if (this.findDeliverableIndex() >= 0) {
        this.scheduleDrain();
      }
    });
  }

  private async drain(): Promise<void> {
    for (let index = this.findDeliverableIndex(); index >= 0; index = this.findDeliverableIndex()) {
      const queued = this.queue[index];
      if (queued === undefined) continue;
      this.queue[index] = undefined;
      this.queuedCount -= 1;
      this.advanceHead();
      this.compactQueue();
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

  private reportDropped(signal?: AbortSignal): void {
    if (this.dropReportPending) {
      return;
    }
    this.dropReportPending = true;
    queueMicrotask(() => {
      void this.events.publishError({
        code: ZlinkStreamErrorCode.ReceivedMessageDropped,
        message: 'Received stream message was dropped because the received-message queue is full.'
      }, signal).finally(() => {
        this.dropReportPending = false;
      }).catch(() => {});
    });
  }
}
