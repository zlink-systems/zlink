// SPDX-License-Identifier: MPL-2.0

import { Message, type MessageLike } from '../../contracts/messaging';
import { consumeSubmittedMessage } from '../../contracts/messaging/message';
import { SubmitError, SubmitResult } from '../../contracts/errors/errors';
import { requireNative } from '../native/native';
import { submitNativeError } from '../errors/native_errors';
import { withRuntimeErrorMessage } from '../errors/error_state';
import { SendFlags } from '../../contracts/sockets/socket_constants';
import type { OperationPayloadValue } from '../messaging/send_operation_base';

const SYNTHETIC_ERRNO = 0;
const MAX_READY_ATTEMPTS_PER_TURN = 64;

interface PendingPublish {
  readonly topic: string;
  readonly payload: readonly Buffer[];
  readonly messages: readonly Message[];
  readonly deadline: number;
  resolve(): void;
  reject(error: unknown): void;
  timer?: NodeJS.Timeout;
  terminal: boolean;
}

function copiedPayload(payload: OperationPayloadValue<MessageLike>): {
  readonly payload: readonly Buffer[];
  readonly messages: readonly Message[];
} {
  const values = Array.isArray(payload) ? payload : [payload];
  const frames: Buffer[] = [];
  const messages: Message[] = [];
  for (const value of values) {
    if (value instanceof Message) {
      frames.push(value.toBytes());
      messages.push(value);
    } else if (Buffer.isBuffer(value)) frames.push(Buffer.from(value));
    else if (value instanceof Uint8Array) frames.push(Buffer.from(value));
    else if (typeof value === 'string') frames.push(Buffer.from(value));
    else throw new TypeError('publish payload must be Message, Buffer, Uint8Array, or string');
  }
  return { payload: frames, messages };
}

function failure(result: SubmitResult, message: string): SubmitError {
  return withRuntimeErrorMessage(new SubmitError(result, SYNTHETIC_ERRNO), message);
}

/**
 * Binding-owned PUB admission. It owns no byte/count budget: Core remains the
 * sole HWM owner. A send-ready edge drains a bounded round-robin pass so one
 * parked publisher cannot monopolize the Node turn.
 */
export class PublisherAdmission {
  private readonly native = requireNative();
  // New submissions and HWM-parked records are deliberately separate. A new
  // record always gets one immediate DONTWAIT attempt; a parked record moves
  // only on Core's next send-ready edge.
  private readonly fresh: PendingPublish[] = [];
  private readonly parked: PendingPublish[] = [];
  private initialScheduled = false;
  private readyScheduled = false;
  private pumping = false;
  private closed = false;
  private observer: (() => void) | undefined;

  constructor(
    private readonly handle: unknown,
    private readonly socketTimeoutMs: () => number
  ) {
    this.native.socketSendReadyHandler(handle, () => this.onReady());
  }

  submit(
    topic: string,
    payload: OperationPayloadValue<MessageLike>,
    requestedTimeoutMs: number,
    startedAt: number
  ): Promise<void> {
    if (this.closed) return Promise.reject(failure(SubmitResult.Terminated, 'publish after socket close'));
    const snapshot = copiedPayload(payload);
    const timeoutMs = requestedTimeoutMs === 0 ? this.socketTimeoutMs() : requestedTimeoutMs;
    const deadline = timeoutMs < 0 ? Number.POSITIVE_INFINITY : startedAt + timeoutMs;
    let pending!: PendingPublish;
    const promise = new Promise<void>((resolve, reject) => {
      pending = { topic, ...snapshot, deadline, resolve, reject, terminal: false };
    });
    if (Number.isFinite(deadline)) {
      pending.timer = setTimeout(() => this.timeout(pending), Math.max(0, deadline - Date.now()));
      pending.timer.unref();
    }
    this.fresh.push(pending);
    this.scheduleInitial();
    return promise;
  }

  close(): void {
    if (this.closed) return;
    this.closed = true;
    for (const pending of [...this.fresh, ...this.parked]) {
      if (pending) this.reject(pending, failure(SubmitResult.Terminated, 'publish terminated because socket closed'));
    }
    this.fresh.length = 0;
    this.parked.length = 0;
  }

  setObserver(observer: () => void): void {
    if (this.observer !== undefined) {
      throw new Error('send-ready handler already attached');
    }
    this.observer = observer;
  }

  private onReady(): void {
    // One native callback slot is shared by admission and the public observer;
    // installing a second handler would silently lose the binding wake path.
    this.scheduleReady();
    this.observer?.();
  }

  private scheduleInitial(): void {
    if (this.closed || this.initialScheduled) return;
    this.initialScheduled = true;
    queueMicrotask(() => this.pump(false));
  }

  private scheduleReady(): void {
    if (this.closed || this.readyScheduled) return;
    this.readyScheduled = true;
    queueMicrotask(() => this.pump(true));
  }

  private pump(readyPass: boolean): void {
    if (this.closed || this.pumping) return;
    if (readyPass) this.readyScheduled = false;
    else this.initialScheduled = false;
    this.pumping = true;
    try {
      const source = readyPass ? this.parked : this.fresh;
      // Snapshot the queue at turn start. A record is attempted at most once
      // in this pass, so [A blocked, B ready] cannot become head-of-line.
      const attempts = Math.min(source.length, MAX_READY_ATTEMPTS_PER_TURN);
      for (let index = 0; index < attempts; index += 1) {
        const pending = source.shift();
        if (!pending || pending.terminal) continue;
        if (Number.isFinite(pending.deadline) && Date.now() >= pending.deadline) {
          this.timeout(pending);
          continue;
        }
        if (this.attempt(pending)) {
          continue;
        }
        // Backpressure is Core's parking state, never a Framework capacity.
        this.parked.push(pending);
      }
    } finally {
      this.pumping = false;
    }
    // More fresh records must receive their first attempt without waiting for
    // an unrelated HWM edge. Parked records intentionally do not reschedule.
    if (this.fresh.length > 0) this.scheduleInitial();
    if (this.readyScheduled) this.scheduleReady();
  }

  /** true means terminal; false means Core backpressured and must park. */
  private attempt(pending: PendingPublish): boolean {
    let result: number;
    try {
      result = this.native.socketTryPublish(this.handle, pending.topic, pending.payload) as number;
    } catch (error) {
      this.reject(pending, submitNativeError(error, SendFlags.DontWait, 'publish failed'));
      return true;
    }
    if (result === SubmitResult.Ok) {
      this.accept(pending);
      return true;
    }
    if (result === SubmitResult.Backpressured) return false;
    this.reject(pending, failure(result as SubmitResult, 'publish failed'));
    return true;
  }

  private accept(pending: PendingPublish): void {
    if (pending.terminal) return;
    pending.terminal = true;
    if (pending.timer) clearTimeout(pending.timer);
    for (const message of pending.messages) consumeSubmittedMessage(message);
    pending.resolve();
  }

  private timeout(pending: PendingPublish): void {
    this.reject(pending, failure(SubmitResult.Backpressured, 'publish timed out while waiting for Core admission'));
  }

  private reject(pending: PendingPublish, error: unknown): void {
    if (pending.terminal) return;
    pending.terminal = true;
    if (pending.timer) clearTimeout(pending.timer);
    pending.reject(error);
  }
}
