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
import { PollEventFlag } from '../../contracts/sockets/socket_constants';

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

interface SendRetryState {
  readonly nativePayload: ReturnType<typeof normalizeOperationPayload>;
  readonly routingId: Buffer | null;
}

const RESOLVED_SEND = Promise.resolve();
const MAX_IDLE_PUMP_DELAY_MS = 8;

/** @internal Exponential event-loop backoff for the Core API's pull-only queue. */
export function completionPumpDelayMs(idlePolls: number): number {
  if (idlePolls <= 0) return 0;
  return Math.min(MAX_IDLE_PUMP_DELAY_MS, 2 ** Math.min(idlePolls - 1, 3));
}

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
  private readonly sendRetries = new Map<bigint, SendRetryState>();
  private nextToken = 1n;
  private publicOwner: object | null = null;
  private pumpScheduled = false;
  private idlePumpPolls = 0;
  private pumpHandle: ReturnType<typeof setImmediate> | ReturnType<typeof setTimeout> | null = null;
  private pumpUsesTimeout = false;
  private runtimePoller: NativeHandle | null = null;
  private runtimeEvents: NativeHandle | null = null;
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
    this.sendRetries.set(token, {
      nativePayload: retryPayload,
      routingId: routingId === null ? null : Buffer.from(routingId),
    });
    this.managedWritableWaitCount += 1;
    this.schedulePump();
    return entry.promise;
  }

  submitRequest(
    payload: OperationPayloadValue<MessageLike>,
    target: Buffer | null,
    timeoutMs: number
  ): Promise<Message[]> {
    const entry = this.register<Message[]>('request');
    let result: NativeSubmitResult;
    try {
      result = this.native.socketSubmitRequest(
        this.handle,
        target,
        normalizeOperationPayload(payload),
        timeoutMs,
        1,
        entry.token
      ) as NativeSubmitResult;
    } catch (error) {
      this.failEntry(entry, submitNativeError(error, 1, 'request submit failed'));
      return entry.promise;
    }
    if (result.result !== SubmitResult.Ok) {
      this.failEntry(entry, submitError(result.result, result.nativeErrno, 'request submit failed'));
      return entry.promise;
    }
    consumeSubmittedMessages(payload);
    this.publish(entry, result.completionId);
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
    // Core completion polling has a single owner. Drop the lazy runtime
    // registration before the public poller attempts to acquire it.
    try {
      this.closeRuntimePoller(true);
    } catch (error) {
      this.schedulePump();
      throw error;
    }
    this.publicOwner = owner;
    return true;
  }

  transferToRuntime(owner: object): void {
    if (this.publicOwner !== owner) return;
    this.publicOwner = null;
    this.idlePumpPolls = 0;
    this.schedulePump();
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
    this.sendRetries.clear();
    this.managedWritableWaitCount = 0;
    if (this.pumpHandle !== null) {
      if (this.pumpUsesTimeout) {
        clearTimeout(this.pumpHandle as ReturnType<typeof setTimeout>);
      } else {
        clearImmediate(this.pumpHandle as ReturnType<typeof setImmediate>);
      }
      this.pumpHandle = null;
      this.pumpScheduled = false;
    }
    this.closeRuntimePoller();
  }

  private register<T>(
    kind: CompletionKind,
    schedule = true,
    expectsUserContext = true
  ): CompletionEntry<T> {
    if (this.closed) throw submitError(SubmitResult.InvalidState, 0, 'socket is closed');
    const entry = new CompletionEntry<T>(this.nextToken++, kind, expectsUserContext);
    this.byToken.set(entry.token, entry as CompletionEntry<unknown>);
    if (schedule) this.schedulePump();
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
    if (entry.kind === 'send') {
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
    const retry = this.sendRetries.get(entry.token);
    const sameTarget = retry !== undefined
      && (retry.routingId === null
        ? completion.peerRoutingId === null
        : completion.peerRoutingId !== null
          && retry.routingId.equals(completion.peerRoutingId));
    if (completion.kind !== COMPLETION_WRITABLE
        || completion.completionId !== entry.completionId
        || completion.userContext !== entry.token
        || !sameTarget) {
      this.failEntry(entry, submitError(
        SubmitResult.InternalError,
        completion.terminalErrno,
        'writable completion did not match the pending send'
      ));
      return;
    }

    this.byId.delete(entry.completionId);
    if (completion.sendResult !== SEND_ADMITTED) {
      const error = completion.sendResult === SEND_TERMINAL
        ? createError('submit', completion.terminalErrno, 'send target became unavailable')
        : submitError(
            SubmitResult.InternalError,
            completion.terminalErrno,
            'writable completion result mismatch'
          );
      this.failEntry(entry, error);
      return;
    }
    this.attemptSend(entry);
  }

  private attemptSend(entry: CompletionEntry<unknown>): void {
    const retry = this.sendRetries.get(entry.token);
    if (!retry) {
      this.failEntry(entry, submitError(
        SubmitResult.InternalError,
        0,
        'pending send payload is missing'
      ));
      return;
    }

    let result: NativeSubmitResult;
    try {
      result = this.native.socketSubmitSend(
        this.handle,
        retry.nativePayload,
        retry.routingId,
        DONTWAIT,
        entry.token
      ) as NativeSubmitResult;
    } catch (error) {
      this.failEntry(entry, submitNativeError(error, DONTWAIT, 'send submit failed'));
      return;
    }

    if (result.result === SubmitResult.Ok) {
      if (result.completionId !== 0n) {
        this.failEntry(entry, submitError(
          SubmitResult.InternalError,
          result.nativeErrno,
          'successful send returned a completion token'
        ));
        return;
      }
      entry.succeed(undefined);
      this.removeIfSettled(entry);
      return;
    }

    if (result.result === SubmitResult.Backpressured) {
      if (!isWouldBlock(result.nativeErrno) || result.completionId === 0n) {
        this.failEntry(entry, submitError(
          SubmitResult.Backpressured,
          result.nativeErrno,
          'backpressured send did not return an EAGAIN wait token'
        ));
        return;
      }
      if (entry.completionId !== 0n) this.byId.delete(entry.completionId);
      entry.awaitWritable(result.completionId);
      this.byId.set(result.completionId, entry);
      this.schedulePump();
      return;
    }

    this.failEntry(entry, submitError(
      result.result,
      result.nativeErrno,
      'send submit failed'
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
    if (this.sendRetries.delete(entry.token)) {
      this.managedWritableWaitCount -= 1;
    }
  }

  private schedulePump(): void {
    if (this.closed || this.publicOwner || this.pumpScheduled || this.byToken.size === 0) return;
    this.pumpScheduled = true;
    const run = (): void => {
      this.pumpScheduled = false;
      this.pumpHandle = null;
      if (this.closed || this.publicOwner || this.byToken.size === 0) return;
      let processed = 0;
      try {
        processed = this.pollAndDrain();
      } catch (error) {
        const nativeErrno = readErrno();
        const message = nativeErrorMessage(error, 'completion poll failed');
        for (const entry of this.byToken.values()) {
          entry.fail(entry.kind === 'request'
            ? (nativeErrno === 0
                ? requestError(RequestResult.InternalError, message)
                : createError('request', nativeErrno, message))
            : (nativeErrno === 0
                ? submitError(SubmitResult.InternalError, 0, message)
                : createError('submit', nativeErrno, message)));
        }
        this.byToken.clear();
        this.byId.clear();
        this.sendRetries.clear();
        this.managedWritableWaitCount = 0;
        this.closeRuntimePoller();
        return;
      }
      this.idlePumpPolls = processed === 0
        ? Math.min(this.idlePumpPolls + 1, 4)
        : 0;
      if (this.byToken.size > 0) {
        this.schedulePump();
      } else {
        this.closeRuntimePoller();
      }
    };
    const delayMs = completionPumpDelayMs(this.idlePumpPolls);
    this.pumpUsesTimeout = delayMs !== 0;
    this.pumpHandle = delayMs === 0
      ? setImmediate(run)
      : setTimeout(run, delayMs);
  }

  private pollAndDrain(): number {
    this.ensureRuntimePoller();
    const count = this.native.pollerWaitInto(
      this.runtimePoller,
      this.runtimeEvents,
      1,
      0
    ) as number;
    if (count <= 0) return 0;
    const events = this.native.pollEventsRevents(this.runtimeEvents, 0) as number;
    if ((events & (PollEventFlag.PollOut | PollEventFlag.PollCompletion)) !== 0) {
      return this.drain();
    }
    return 0;
  }

  private ensureRuntimePoller(): void {
    if (this.runtimePoller !== null && this.runtimeEvents !== null) return;
    const poller = this.native.pollerNew() as NativeHandle;
    let events: NativeHandle | null = null;
    try {
      events = this.native.pollEventsNew(1) as NativeHandle;
      this.native.pollerAdd(
        poller,
        this.handle,
        null,
        PollEventFlag.PollOut | PollEventFlag.PollCompletion
      );
    } catch (error) {
      if (events !== null) {
        try { this.native.pollEventsDestroy(events); } catch { /* best-effort cleanup */ }
      }
      try { this.native.pollerDestroy(poller); } catch { /* best-effort cleanup */ }
      throw error;
    }
    this.runtimePoller = poller;
    this.runtimeEvents = events;
  }

  private closeRuntimePoller(reportFailure = false): void {
    const poller = this.runtimePoller;
    const events = this.runtimeEvents;
    if (poller !== null) {
      try {
        this.native.pollerDestroy(poller);
        this.runtimePoller = null;
      } catch (error) {
        if (reportFailure) throw error;
        this.runtimePoller = null;
      }
    }
    if (events !== null) {
      try { this.native.pollEventsDestroy(events); } catch { /* best-effort cleanup */ }
      this.runtimeEvents = null;
    }
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
