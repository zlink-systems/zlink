import type { Message } from '../../contracts/Common/Message';
import { copyMessage } from './stream-message-utils';

export class ZLinkPendingSessionRequest {
  private completed = false;
  private readonly timeout: ReturnType<typeof setTimeout> | undefined;
  private resolvePromise!: (message: Message) => void;
  private rejectPromise!: (error: unknown) => void;
  readonly promise: Promise<Message>;

  constructor(
    private readonly tracker: ZLinkSessionRequestTracker,
    readonly requestSeq: bigint,
    timeoutMs: number | undefined
  ) {
    this.promise = new Promise<Message>((resolve, reject) => {
      this.resolvePromise = resolve;
      this.rejectPromise = reject;
    });
    if (timeoutMs !== undefined) {
      this.timeout = setTimeout(() => this.cancel(), timeoutMs);
    }
  }

  complete(payload: Message): void {
    if (this.completed) {
      return;
    }
    this.completed = true;
    this.clearTimeout();
    this.resolvePromise(copyMessage(payload));
  }

  fail(error: unknown): void {
    if (this.completed) {
      return;
    }
    this.completed = true;
    this.clearTimeout();
    this.rejectPromise(error);
  }

  cancel(): void {
    if (this.completed) {
      return;
    }
    this.completed = true;
    this.clearTimeout();
    this.tracker.remove(this.requestSeq);
    this.rejectPromise(new Error('Client stream request timed out.'));
  }

  dispose(): void {
    this.clearTimeout();
    this.tracker.remove(this.requestSeq);
  }

  private clearTimeout(): void {
    if (this.timeout !== undefined) {
      clearTimeout(this.timeout);
    }
  }
}

export class ZLinkSessionRequestTracker {
  private readonly pending = new Map<bigint, ZLinkPendingSessionRequest>();
  private nextRequestSeq = 0n;

  start(timeoutMs?: number): ZLinkPendingSessionRequest {
    const requestSeq = this.next();
    const pending = new ZLinkPendingSessionRequest(this, requestSeq, timeoutMs);
    if (this.pending.has(requestSeq)) {
      throw new Error('Duplicate stream request sequence.');
    }
    this.pending.set(requestSeq, pending);
    return pending;
  }

  complete(requestSeq: bigint, payload: Message): boolean {
    const pending = this.pending.get(requestSeq);
    if (pending === undefined) {
      return false;
    }
    this.pending.delete(requestSeq);
    pending.complete(payload);
    return true;
  }

  fail(requestSeq: bigint, error: unknown): boolean {
    const pending = this.pending.get(requestSeq);
    if (pending === undefined) {
      return false;
    }
    this.pending.delete(requestSeq);
    pending.fail(error);
    return true;
  }

  remove(requestSeq: bigint): void {
    this.pending.delete(requestSeq);
  }

  private next(): bigint {
    do {
      this.nextRequestSeq = (this.nextRequestSeq + 1n) & 0xffffffffffffffffn;
    } while (this.nextRequestSeq === 0n);
    return this.nextRequestSeq;
  }
}
