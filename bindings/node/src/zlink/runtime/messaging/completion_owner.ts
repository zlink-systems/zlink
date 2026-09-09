// SPDX-License-Identifier: MPL-2.0

import { Message, type MessageLike } from '../../contracts';
import {
  RequestError,
  RequestResult,
  SubmitError,
  SubmitResult,
} from '../../contracts/errors/errors';
import { consumeSubmittedMessage } from '../../contracts/messaging/message';
import { normalizeOperationPayload } from '../buffers/message_conversion';
import { createError } from '../errors/error_mapping';
import { withRuntimeErrorMessage } from '../errors/error_state';
import {
  isWouldBlock,
  nativeErrorMessage,
  readErrno,
  submitNativeError,
} from '../errors/native_errors';
import type { NativeHandle } from '../native/binding_types';
import { requireNative } from '../native/native';
import type { OperationPayloadValue } from './send_operation_base';
import { messagesFromNativeBuffers } from './request_executor';

const COMPLETION_REQUEST = 2;
const COMPLETION_WRITABLE = 3;
const SEND_ADMITTED = 0;
const SEND_TERMINAL = 202;
const DONTWAIT = 1;

export interface NativeCompletion {
  readonly kind: number;
  readonly completionId: bigint;
  readonly userContext: bigint;
  readonly peerRoutingId: Buffer | null;
  readonly sendResult: number;
  readonly terminalErrno: number;
  readonly requestResult: number;
  readonly parts?: Buffer[];
}

export interface NativeSubmitResult {
  readonly result: number;
  readonly nativeErrno: number;
  readonly completionId: bigint;
}

export interface NativeSyncRequestResult extends NativeSubmitResult {
  readonly completions: NativeCompletion[];
}

type CompletionKind = 'send' | 'request';

interface RetryState {
  readonly kind: CompletionKind;
  readonly nativePayload: ReturnType<typeof normalizeOperationPayload>;
  readonly routingId: Buffer | null;
  readonly timeoutMs: number;
}

const RESOLVED_SEND = Promise.resolve();

function snapshotRetryPart(part: MessageLike): Buffer {
  return part instanceof Message ? part.toBytes() : Buffer.from(part);
}

function snapshotRetryPayload(
  payload: OperationPayloadValue<MessageLike>
): ReturnType<typeof normalizeOperationPayload> {
  if (!Array.isArray(payload)) return snapshotRetryPart(payload as MessageLike);
  if (payload.length === 1) return snapshotRetryPart(payload[0]);
  return payload.map(snapshotRetryPart);
}

function submitError(result: number, nativeErrno: number, message: string): SubmitError {
  return withRuntimeErrorMessage(
    new SubmitError(result as SubmitResult, nativeErrno),
    message
  );
}

function requestError(result: number, message: string): RequestError {
  return withRuntimeErrorMessage(new RequestError(result as RequestResult), message);
}

export function consumeSubmittedMessages(payload: OperationPayloadValue<MessageLike>): void {
  if (payload instanceof Message) {
    consumeSubmittedMessage(payload);
  } else if (Array.isArray(payload)) {
    for (const part of payload) {
      if (part instanceof Message) consumeSubmittedMessage(part);
    }
  }
}

/** Two-phase correlation entry: publish and capture may arrive in either order. */
export class CompletionEntry<T> {
  readonly token: bigint;
  readonly kind: CompletionKind;
  readonly expectsUserContext: boolean;
  readonly promise: Promise<T>;
  completionId = 0n;
  published = false;
  captured = false;
  settled = false;
  value: T | undefined;
  error: unknown;
  private capturedCompletionId = 0n;
  private resolvePromise!: (value: T) => void;
  private rejectPromise!: (error: unknown) => void;

  constructor(token: bigint, kind: CompletionKind, expectsUserContext = true) {
    this.token = token;
    this.kind = kind;
    this.expectsUserContext = expectsUserContext;
    this.promise = new Promise<T>((resolve, reject) => {
      this.resolvePromise = resolve;
      this.rejectPromise = reject;
    });
  }

  publish(completionId: bigint): void {
    this.completionId = completionId;
    this.published = true;
    this.validateRequestCorrelation();
    this.settleIfJoined();
  }

  awaitWritable(completionId: bigint): void {
    this.completionId = completionId;
    this.published = true;
    this.captured = false;
  }

  succeed(value: T): void {
    if (this.settled) return;
    this.published = true;
    this.captured = true;
    this.value = value;
    this.settleIfJoined();
  }

  capture(completion: NativeCompletion): void {
    if (this.captured) return;
    this.capturedCompletionId = completion.completionId;
    try {
      if (this.kind !== 'request' || completion.kind !== COMPLETION_REQUEST) {
        throw requestError(RequestResult.InternalError, 'request completion kind mismatch');
      }
      const expectedContext = this.expectsUserContext ? this.token : 0n;
      if (completion.userContext !== expectedContext) {
        throw requestError(RequestResult.InternalError, 'request completion context mismatch');
      }
      if (completion.requestResult !== RequestResult.Ok) {
        throw requestError(completion.requestResult, 'request failed');
      }
      this.value = messagesFromNativeBuffers(completion.parts) as T;
    } catch (error) {
      this.error = error;
    }
    this.captured = true;
    this.validateRequestCorrelation();
    this.settleIfJoined();
  }

  fail(error: unknown): void {
    if (this.settled) return;
    this.published = true;
    this.captured = true;
    this.error = error;
    this.settleIfJoined();
  }

  private settleIfJoined(): void {
    if (!this.published || !this.captured || this.settled) return;
    this.settled = true;
    if (this.error !== undefined) this.rejectPromise(this.error);
    else this.resolvePromise(this.value as T);
  }

  private validateRequestCorrelation(): void {
    if (this.kind !== 'request' || !this.published || !this.captured) return;
    if (this.capturedCompletionId !== this.completionId) {
      this.error = requestError(
        RequestResult.InternalError,
        'request completion id mismatch'
      );
    }
  }
}

const owners = new WeakMap<object, CompletionOwner>();

export class CompletionOwner {
  private readonly native = requireNative();
  private readonly byToken = new Map<bigint, CompletionEntry<unknown>>();
  private readonly byId = new Map<bigint, CompletionEntry<unknown>>();
  private readonly retries = new Map<bigint, RetryState>();
  private readonly writableRetries: CompletionEntry<unknown>[] = [];
  private nextToken = 1n;
  private publicOwner: object | null = null;
  private runtimeWatch: NativeHandle | null = null;
  private managedWritableWaitCount = 0;
  private closed = false;

  constructor(private readonly handle: NativeHandle) {}

  submitSend(
    payload: OperationPayloadValue<MessageLike>,
    routingId: Buffer | null
  ): Promise<void> {
    if (this.closed) throw submitError(SubmitResult.InvalidState, 0, 'socket is closed');
    const token = this.nextToken++;
    let nativePayload: ReturnType<typeof normalizeOperationPayload>;
    try {
      nativePayload = normalizeOperationPayload(payload);
    } catch (error) {
      return Promise.reject(submitNativeError(error, DONTWAIT, 'send submit failed'));
    }

    let result: NativeSubmitResult;
    try {
      result = this.native.socketSubmitSend(
        this.handle,
        nativePayload,
        routingId,
        DONTWAIT,
        token
      ) as NativeSubmitResult;
    } catch (error) {
      return Promise.reject(submitNativeError(error, DONTWAIT, 'send submit failed'));
    }

    if (result.result === SubmitResult.Ok) {
      try {
        consumeSubmittedMessages(payload);
      } catch (error) {
        return Promise.reject(error);
      }
      if (result.completionId !== 0n) {
        return Promise.reject(submitError(
          SubmitResult.InternalError,
          result.nativeErrno,
          'successful send returned a completion token'
        ));
      }
      return RESOLVED_SEND;
    }

    if (result.result !== SubmitResult.Backpressured) {
      return Promise.reject(submitError(
        result.result,
        result.nativeErrno,
        'send submit failed'
      ));
    }
    if (!isWouldBlock(result.nativeErrno) || result.completionId === 0n) {
      return Promise.reject(submitError(
        SubmitResult.Backpressured,
        result.nativeErrno,
        'backpressured send did not return an EAGAIN wait token'
      ));
    }

    let retryPayload: ReturnType<typeof normalizeOperationPayload>;
    try {
      // Core retains no SEND payload. Copy only after actual rejection, before
      // returning control to user code, so later caller mutation is invisible.
      retryPayload = snapshotRetryPayload(payload);
      consumeSubmittedMessages(payload);
    } catch (error) {
      return Promise.reject(error);
    }

    const entry = new CompletionEntry<void>(token, 'send', false);
    entry.awaitWritable(result.completionId);
    this.byToken.set(token, entry as CompletionEntry<unknown>);
    this.byId.set(result.completionId, entry as CompletionEntry<unknown>);
    this.retries.set(token, {
      kind: 'send',
      nativePayload: retryPayload,
      routingId: routingId === null ? null : Buffer.from(routingId),
      timeoutMs: 0,
    });
    this.managedWritableWaitCount += 1;
    this.ensureRuntimeWatch();
    return entry.promise;
  }

  submitRequest(
    payload: OperationPayloadValue<MessageLike>,
    target: Buffer | null,
    timeoutMs: number
  ): Promise<Message[]> {
    if (this.closed) throw submitError(SubmitResult.InvalidState, 0, 'socket is closed');
    const token = this.nextToken++;
    let nativePayload: ReturnType<typeof normalizeOperationPayload>;
    try {
      nativePayload = normalizeOperationPayload(payload);
    } catch (error) {
      return Promise.reject(submitNativeError(error, DONTWAIT, 'request submit failed'));
    }

    let result: NativeSubmitResult;
    try {
      result = this.native.socketSubmitRequest(
        this.handle,
        target,
        nativePayload,
        timeoutMs,
        DONTWAIT,
        token
      ) as NativeSubmitResult;
    } catch (error) {
      return Promise.reject(submitNativeError(error, DONTWAIT, 'request submit failed'));
    }

    if (result.result === SubmitResult.Ok) {
      if (result.completionId === 0n) {
        return Promise.reject(submitError(
          SubmitResult.InternalError,
          result.nativeErrno,
          'successful request returned no completion id'
        ));
      }
      try {
        consumeSubmittedMessages(payload);
      } catch (error) {
        return Promise.reject(error);
      }
      const entry = this.createEntry<Message[]>('request', token);
      this.publish(entry, result.completionId);
      this.ensureRuntimeWatch();
      return entry.promise;
    }

    if (result.result !== SubmitResult.Backpressured) {
      return Promise.reject(submitError(
        result.result,
        result.nativeErrno,
        'request submit failed'
      ));
    }
    if (!isWouldBlock(result.nativeErrno) || result.completionId === 0n) {
      return Promise.reject(submitError(
        SubmitResult.Backpressured,
        result.nativeErrno,
        'backpressured request did not return an EAGAIN wait token'
      ));
    }

    let retryPayload: ReturnType<typeof normalizeOperationPayload>;
    try {
      // Core retains no REQUEST payload before admission. Snapshot only after
      // refusal, then release the caller-visible Message wrappers immediately.
      retryPayload = snapshotRetryPayload(payload);
      consumeSubmittedMessages(payload);
    } catch (error) {
      return Promise.reject(error);
    }

    const entry = this.createEntry<Message[]>('request', token);
    entry.awaitWritable(result.completionId);
    this.byId.set(result.completionId, entry as CompletionEntry<unknown>);
    this.retries.set(token, {
      kind: 'request',
      nativePayload: retryPayload,
      routingId: target === null ? null : Buffer.from(target),
      timeoutMs,
    });
    this.managedWritableWaitCount += 1;
    this.ensureRuntimeWatch();
    return entry.promise;
  }

  requestSync(
    payload: OperationPayloadValue<MessageLike>,
    target: Buffer | null,
    timeoutMs: number
  ): Message[] {
    const entry = this.register<Message[]>('request', false, false);
    void entry.promise.catch(() => {});
    let result: NativeSyncRequestResult;
    try {
      result = this.native.socketRequestSync(
        this.handle,
        target,
        normalizeOperationPayload(payload),
        timeoutMs
      ) as NativeSyncRequestResult;
    } catch (error) {
      this.failEntry(entry, submitNativeError(error, 0, 'request submit failed'));
      throw entry.error;
    }
    if (result.result !== SubmitResult.Ok) {
      this.failEntry(entry, submitError(result.result, result.nativeErrno, 'request submit failed'));
      throw entry.error;
    }
    consumeSubmittedMessages(payload);
    this.publish(entry, result.completionId);
    for (const completion of result.completions) this.capture(completion);
    this.drain();
    if (!entry.settled) {
      this.failEntry(entry, requestError(RequestResult.InternalError, 'request completion missing'));
    }
    if (entry.error !== undefined) throw entry.error;
    return entry.value ?? [];
  }

  sendSync(payload: OperationPayloadValue<MessageLike>, routingId: Buffer | null): void {
    let result: NativeSubmitResult;
    try {
      result = this.native.socketSubmitSend(
        this.handle,
        normalizeOperationPayload(payload),
        routingId,
        0,
        0n
      ) as NativeSubmitResult;
    } catch (error) {
      throw submitNativeError(error, 0, 'send failed');
    }
    if (result.result !== SubmitResult.Ok) {
      throw submitError(result.result, result.nativeErrno, 'send failed');
    }
    consumeSubmittedMessages(payload);
    if (result.completionId !== 0n) {
      throw submitError(
        SubmitResult.InternalError,
        result.nativeErrno,
        'successful synchronous send returned a completion token'
      );
    }
  }

  transferToPublic(owner: object): boolean {
    if (this.closed) throw submitError(SubmitResult.InvalidState, 0, 'socket is closed');
    if (this.publicOwner && this.publicOwner !== owner) {
      throw submitError(SubmitResult.InvalidState, 0, 'completion owner already transferred');
    }
    if (this.publicOwner === owner) return false;
    // Stop the event-loop fd watcher before the public Core poller starts
    // observing the same socket notification source.
    this.closeRuntimeWatch();
    this.publicOwner = owner;
    return true;
  }

  transferToRuntime(owner: object): void {
    if (this.publicOwner !== owner) return;
    this.publicOwner = null;
    this.ensureRuntimeWatch();
  }

  drain(caller?: object): number {
    if (this.publicOwner !== null && this.publicOwner !== caller) return 0;
    let processed = 0;
    for (;;) {
      const completion = this.native.socketCompletionRecv(this.handle, 1) as NativeCompletion | null;
      if (!completion) break;
      this.capture(completion);
      processed += 1;
    }
    // Resubmission can enqueue another WRITABLE immediately. It belongs to
    // the next drain, after this queue has reached NO_DATA.
    try {
      for (const entry of this.writableRetries) this.attemptRetry(entry);
    } finally {
      this.writableRetries.length = 0;
    }
    return processed;
  }

  hasManagedWritableWait(): boolean {
    return this.managedWritableWaitCount !== 0;
  }

  close(): void {
    if (this.closed) return;
    this.closed = true;
    this.publicOwner = null;
    for (const entry of this.byToken.values()) {
      entry.fail(entry.kind === 'request'
        ? requestError(RequestResult.Terminated, 'socket closed')
        : submitError(SubmitResult.Terminated, 0, 'socket closed'));
    }
    this.byToken.clear();
    this.byId.clear();
    this.retries.clear();
    this.writableRetries.length = 0;
    this.managedWritableWaitCount = 0;
    this.closeRuntimeWatch();
  }

  private register<T>(
    kind: CompletionKind,
    schedule = true,
    expectsUserContext = true
  ): CompletionEntry<T> {
    if (this.closed) throw submitError(SubmitResult.InvalidState, 0, 'socket is closed');
    const entry = this.createEntry<T>(kind, this.nextToken++, expectsUserContext);
    if (schedule) this.ensureRuntimeWatch();
    return entry;
  }

  private createEntry<T>(
    kind: CompletionKind,
    token: bigint,
    expectsUserContext = true
  ): CompletionEntry<T> {
    const entry = new CompletionEntry<T>(token, kind, expectsUserContext);
    this.byToken.set(entry.token, entry as CompletionEntry<unknown>);
    return entry;
  }

  private publish(entry: CompletionEntry<unknown>, completionId: bigint): void {
    entry.publish(completionId);
    if (completionId !== 0n) this.byId.set(completionId, entry);
    this.removeIfSettled(entry);
  }

  private capture(completion: NativeCompletion): void {
    const entry = completion.userContext !== 0n
      ? this.byToken.get(completion.userContext)
      : this.byId.get(completion.completionId);
    if (!entry) return; // Native already closed the unclaimed completion.
    if (this.retries.has(entry.token)) {
      this.captureWritable(entry, completion);
      return;
    }
    entry.capture(completion);
    this.removeIfSettled(entry);
  }

  private captureWritable(
    entry: CompletionEntry<unknown>,
    completion: NativeCompletion
  ): void {
    if (completion.kind !== COMPLETION_WRITABLE
        || completion.completionId !== entry.completionId
        || completion.userContext !== entry.token) {
      this.failEntry(entry, submitError(
        SubmitResult.InternalError,
        completion.terminalErrno,
        'writable completion did not match the pending operation'
      ));
      return;
    }

    this.byId.delete(entry.completionId);
    if (completion.sendResult !== SEND_ADMITTED) {
      const error = completion.sendResult === SEND_TERMINAL
        ? createError('submit', completion.terminalErrno, 'submit target became unavailable')
        : submitError(
            SubmitResult.InternalError,
            completion.terminalErrno,
            'writable completion result mismatch'
          );
      this.failEntry(entry, error);
      return;
    }
    this.writableRetries.push(entry);
  }

  private attemptRetry(entry: CompletionEntry<unknown>): void {
    const retry = this.retries.get(entry.token);
    if (!retry) {
      this.failEntry(entry, submitError(
        SubmitResult.InternalError,
        0,
        'pending operation payload is missing'
      ));
      return;
    }

    let result: NativeSubmitResult;
    try {
      result = retry.kind === 'send'
        ? this.native.socketSubmitSend(
            this.handle,
            retry.nativePayload,
            retry.routingId,
            DONTWAIT,
            entry.token
          ) as NativeSubmitResult
        : this.native.socketSubmitRequest(
            this.handle,
            retry.routingId,
            retry.nativePayload,
            retry.timeoutMs,
            DONTWAIT,
            entry.token
          ) as NativeSubmitResult;
    } catch (error) {
      this.failEntry(entry, submitNativeError(error, DONTWAIT, `${retry.kind} submit failed`));
      return;
    }

    if (result.result === SubmitResult.Ok) {
      const validCompletionId = retry.kind === 'send'
        ? result.completionId === 0n
        : result.completionId !== 0n;
      if (!validCompletionId) {
        this.failEntry(entry, submitError(
          SubmitResult.InternalError,
          result.nativeErrno,
          `successful ${retry.kind} returned an invalid completion id`
        ));
        return;
      }
      this.removeRetry(entry);
      if (retry.kind === 'send') {
        entry.succeed(undefined);
        this.removeIfSettled(entry);
      } else {
        entry.publish(result.completionId);
        this.byId.set(result.completionId, entry);
        this.ensureRuntimeWatch();
      }
      return;
    }

    if (result.result === SubmitResult.Backpressured) {
      if (!isWouldBlock(result.nativeErrno) || result.completionId === 0n) {
        this.failEntry(entry, submitError(
          SubmitResult.Backpressured,
          result.nativeErrno,
          `backpressured ${retry.kind} did not return an EAGAIN wait token`
        ));
        return;
      }
      if (entry.completionId !== 0n) this.byId.delete(entry.completionId);
      entry.awaitWritable(result.completionId);
      this.byId.set(result.completionId, entry);
      this.ensureRuntimeWatch();
      return;
    }

    this.failEntry(entry, submitError(
      result.result,
      result.nativeErrno,
      `${retry.kind} submit failed`
    ));
  }

  private failEntry(entry: CompletionEntry<unknown>, error: unknown): void {
    entry.fail(error);
    this.removeIfSettled(entry);
  }

  private removeIfSettled(entry: CompletionEntry<unknown>): void {
    if (!entry.settled) return;
    this.byToken.delete(entry.token);
    if (entry.completionId !== 0n) this.byId.delete(entry.completionId);
    this.removeRetry(entry);
  }

  private removeRetry(entry: CompletionEntry<unknown>): void {
    if (this.retries.delete(entry.token)) this.managedWritableWaitCount -= 1;
  }

  private ensureRuntimeWatch(): void {
    if (this.closed || this.publicOwner || this.byToken.size === 0
        || this.runtimeWatch !== null) return;
    this.runtimeWatch = this.native.socketReadableWatchStart(
      this.handle,
      (status: number): void => this.runtimeWake(status)
    ) as NativeHandle;
  }

  private runtimeWake(status: number): void {
    if (this.closed || this.publicOwner) return;
    try {
      if (status < 0) throw new Error(`socket readable watch failed (${status})`);
      this.drain();
    } catch (error) {
      const nativeErrno = readErrno();
      const message = nativeErrorMessage(error, 'completion drain failed');
      for (const entry of this.byToken.values()) {
        entry.fail(this.retries.has(entry.token)
          ? (nativeErrno === 0
              ? submitError(SubmitResult.InternalError, 0, message)
              : createError('submit', nativeErrno, message))
          : entry.kind === 'request'
          ? (nativeErrno === 0
              ? requestError(RequestResult.InternalError, message)
              : createError('request', nativeErrno, message))
          : (nativeErrno === 0
              ? submitError(SubmitResult.InternalError, 0, message)
              : createError('submit', nativeErrno, message)));
      }
      this.byToken.clear();
      this.byId.clear();
      this.retries.clear();
      this.writableRetries.length = 0;
      this.managedWritableWaitCount = 0;
    }
    if (this.byToken.size === 0) this.closeRuntimeWatch();
  }

  private closeRuntimeWatch(): void {
    const watch = this.runtimeWatch;
    this.runtimeWatch = null;
    if (watch === null) return;
    try { this.native.socketReadableWatchStop(watch); } catch { /* best-effort cleanup */ }
  }
}

export function installCompletionOwner(socket: object, handle: NativeHandle): CompletionOwner {
  const owner = new CompletionOwner(handle);
  owners.set(socket, owner);
  return owner;
}

export function completionOwnerOf(socket: object): CompletionOwner {
  const owner = owners.get(socket);
  if (!owner) throw new TypeError('socket has no completion owner');
  return owner;
}

export function releaseCompletionOwner(socket: object): void {
  const owner = owners.get(socket);
  if (!owner) return;
  owner.close();
  owners.delete(socket);
}
