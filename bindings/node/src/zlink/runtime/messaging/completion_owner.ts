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
import { withRuntimeErrorMessage } from '../errors/error_state';
import { submitNativeError } from '../errors/native_errors';
import type { NativeHandle } from '../native/binding_types';
import { requireNative } from '../native/native';
import type { OperationPayloadValue } from './send_operation_base';
import { messagesFromNativeBuffers } from './request_executor';

const COMPLETION_SEND = 1;
const COMPLETION_REQUEST = 2;
const SEND_ADMITTED = 0;

export interface NativeCompletion {
  readonly kind: number;
  readonly completionId: bigint;
  readonly userContext: bigint;
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
  readonly promise: Promise<T>;
  completionId = 0n;
  published = false;
  captured = false;
  settled = false;
  value: T | undefined;
  error: unknown;
  private resolvePromise!: (value: T) => void;
  private rejectPromise!: (error: unknown) => void;

  constructor(token: bigint, kind: CompletionKind) {
    this.token = token;
    this.kind = kind;
    this.promise = new Promise<T>((resolve, reject) => {
      this.resolvePromise = resolve;
      this.rejectPromise = reject;
    });
  }

  publish(completionId: bigint): void {
    this.completionId = completionId;
    this.published = true;
    this.settleIfJoined();
  }

  capture(completion: NativeCompletion): void {
    if (this.captured) return;
    try {
      if (this.kind === 'send') {
        if (completion.kind !== COMPLETION_SEND || completion.sendResult !== SEND_ADMITTED) {
          throw submitError(
            SubmitResult.NotAdmitted,
            completion.terminalErrno,
            'send was not admitted'
          );
        }
        this.value = undefined as T;
      } else {
        if (completion.kind !== COMPLETION_REQUEST) {
          throw requestError(RequestResult.InternalError, 'request completion kind mismatch');
        }
        if (completion.requestResult !== RequestResult.Ok) {
          throw requestError(completion.requestResult, 'request failed');
        }
        this.value = messagesFromNativeBuffers(completion.parts) as T;
      }
    } catch (error) {
      this.error = error;
    }
    this.captured = true;
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
}

const owners = new WeakMap<object, CompletionOwner>();

export class CompletionOwner {
  private readonly native = requireNative();
  private readonly byToken = new Map<bigint, CompletionEntry<unknown>>();
  private readonly byId = new Map<bigint, CompletionEntry<unknown>>();
  private nextToken = 1n;
  private publicOwner: object | null = null;
  private pumpScheduled = false;
  private closed = false;

  constructor(private readonly handle: NativeHandle) {}

  submitSend(
    payload: OperationPayloadValue<MessageLike>,
    routingId: Buffer | null
  ): Promise<void> {
    const entry = this.register<void>('send');
    let result: NativeSubmitResult;
    try {
      result = this.native.socketSubmitSend(
        this.handle,
        normalizeOperationPayload(payload),
        routingId,
        1,
        entry.token
      ) as NativeSubmitResult;
    } catch (error) {
      this.failEntry(entry, submitNativeError(error, 1, 'send submit failed'));
      return entry.promise;
    }
    if (result.result !== SubmitResult.Ok) {
      this.failEntry(entry, submitError(result.result, result.nativeErrno, 'send submit failed'));
      return entry.promise;
    }
    consumeSubmittedMessages(payload);
    this.publish(entry, result.completionId);
    if (result.completionId === 0n) {
      this.capture({
        kind: COMPLETION_SEND,
        completionId: 0n,
        userContext: entry.token,
        sendResult: SEND_ADMITTED,
        terminalErrno: 0,
        requestResult: 0,
      });
    }
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
    const entry = this.register<Message[]>('request', false);
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
  }

  transferToPublic(owner: object): void {
    if (this.closed) throw submitError(SubmitResult.InvalidState, 0, 'socket is closed');
    if (this.publicOwner && this.publicOwner !== owner) {
      throw submitError(SubmitResult.InvalidState, 0, 'completion owner already transferred');
    }
    this.publicOwner = owner;
  }

  transferToRuntime(owner: object): void {
    if (this.publicOwner !== owner) return;
    this.publicOwner = null;
    this.schedulePump();
  }

  drain(): number {
    let processed = 0;
    for (;;) {
      const completion = this.native.socketCompletionRecv(this.handle, 1) as NativeCompletion | null;
      if (!completion) break;
      this.capture(completion);
      processed += 1;
    }
    return processed;
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
  }

  private register<T>(kind: CompletionKind, schedule = true): CompletionEntry<T> {
    if (this.closed) throw submitError(SubmitResult.InvalidState, 0, 'socket is closed');
    const entry = new CompletionEntry<T>(this.nextToken++, kind);
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
    entry.capture(completion);
    this.removeIfSettled(entry);
  }

  private failEntry(entry: CompletionEntry<unknown>, error: unknown): void {
    entry.fail(error);
    this.removeIfSettled(entry);
  }

  private removeIfSettled(entry: CompletionEntry<unknown>): void {
    if (!entry.settled) return;
    this.byToken.delete(entry.token);
    if (entry.completionId !== 0n) this.byId.delete(entry.completionId);
  }

  private schedulePump(): void {
    if (this.closed || this.publicOwner || this.pumpScheduled || this.byToken.size === 0) return;
    this.pumpScheduled = true;
    setImmediate(() => {
      this.pumpScheduled = false;
      if (this.closed || this.publicOwner) return;
      try {
        this.drain();
      } catch (error) {
        for (const entry of this.byToken.values()) entry.fail(error);
        this.byToken.clear();
        this.byId.clear();
        return;
      }
      if (this.byToken.size > 0) {
        setTimeout(() => this.schedulePump(), 1);
      }
    });
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
