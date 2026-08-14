// SPDX-License-Identifier: MPL-2.0

import {
  Message,
  consumeSubmittedMessage,
  type MessageLike,
} from '../../contracts/messaging/message';
import {
  RequestError,
  RequestResult,
  SubmitError,
  SubmitResult,
} from '../../contracts/errors/errors';
import type { OperationPayloadValue } from '../messaging/send_operation_base';
import {
  registerNativeRequest,
  releaseNativeRequestDispatcher,
  type NativeRequestRegistration,
} from '../messaging/request_executor';
import { submitNativeError } from '../errors/native_errors';
import { withRuntimeErrorMessage } from '../errors/error_state';
import { mapNativeErrno } from '../errors/error_mapping';
import { requireNative } from '../native/native';
import { SendFlags } from '../../contracts/sockets/socket_constants';

export interface RoutedTarget {
  readonly peerRid: Buffer;
  readonly transportPairId: bigint;
  readonly transportPairGeneration: bigint;
}

// Core defines zero per-call and zero socket request timeout as its 5s
// request/reply default. Resolve it here because the binding must establish
// the same absolute deadline before admission starts.
const CORE_DEFAULT_REQUEST_TIMEOUT_MS = 5_000;

export function resolvedRequestTimeout(
  operationTimeoutMs: number,
  socketTimeoutMs: number
): number {
  if (operationTimeoutMs !== 0) return operationTimeoutMs;
  return socketTimeoutMs === 0 ? CORE_DEFAULT_REQUEST_TIMEOUT_MS : socketTimeoutMs;
}

interface NativeAttemptResult {
  readonly result: number;
  readonly nativeErrno: number;
}

interface NativeTargetResult extends NativeAttemptResult {
  readonly peerRid?: Buffer;
  readonly transportPairId?: bigint;
  readonly transportPairGeneration?: bigint;
}

interface NativeReadyEvent extends RoutedTarget {
  readonly state: number;
  readonly terminalErrno: number;
}

type RoutedRole = 'dealer' | 'router' | 'stream';
type OperationPhase =
  | 'queued'
  | 'waiting'
  | 'attempting'
  | 'accepted'
  | 'terminal';

interface PendingOperation {
  readonly kind: 'send' | 'request';
  readonly selector: Buffer | null;
  readonly exactTarget: RoutedTarget | null;
  readonly nativeParts: readonly Buffer[];
  readonly submittedMessages: readonly Message[];
  readonly deadline: number;
  readonly attemptBeforeExpiry: boolean;
  readonly request?: NativeRequestRegistration;
  target: RoutedTarget | null;
  key: string | null;
  phase: OperationPhase;
  attempted: boolean;
  timer?: NodeJS.Timeout;
  resolve?: () => void;
  reject?: (error: unknown) => void;
}

const ROUTED_SEND_WRITABLE = 1;
const ROUTED_SEND_TERMINAL = 2;
const SYNTHETIC_ERRNO = 0;

function copiedPayload(
  payload: OperationPayloadValue<MessageLike>
): { nativeParts: Buffer[]; submittedMessages: Message[] } {
  const values = Array.isArray(payload)
    ? payload as readonly MessageLike[]
    : [payload as MessageLike];
  const nativeParts = new Array<Buffer>(values.length);
  const submittedMessages: Message[] = [];
  for (let index = 0; index < values.length; index += 1) {
    const value = values[index];
    if (value instanceof Message) {
      nativeParts[index] = value.toBytes();
      submittedMessages.push(value);
    } else if (Buffer.isBuffer(value)) {
      nativeParts[index] = Buffer.from(value);
    } else if (value instanceof Uint8Array) {
      nativeParts[index] = Buffer.from(value);
    } else if (typeof value === 'string') {
      nativeParts[index] = Buffer.from(value);
    } else {
      throw new TypeError(`parts[${index}] must be Message, Buffer, Uint8Array, or string`);
    }
  }
  return { nativeParts, submittedMessages };
}

function consumeAccepted(messages: readonly Message[]): void {
  for (const message of messages) consumeSubmittedMessage(message);
}

function targetKey(target: RoutedTarget): string {
  return `${target.peerRid.length}:${target.peerRid.toString('hex')}:`
    + `${target.transportPairId}:${target.transportPairGeneration}`;
}

function submitFailure(
  result: number,
  nativeErrno: number,
  message: string
): SubmitError {
  return withRuntimeErrorMessage(
    new SubmitError(result as SubmitResult, nativeErrno),
    message
  );
}

function requestTimeoutFailure(): RequestError {
  return withRuntimeErrorMessage(
    new RequestError(RequestResult.TimedOut, SYNTHETIC_ERRNO),
    'request timed out while waiting for routed admission'
  );
}

function terminalSubmitResult(nativeErrno: number): SubmitResult {
  const mapped = mapNativeErrno('submit', nativeErrno) as SubmitResult;
  return mapped === SubmitResult.NotFound
      || mapped === SubmitResult.NotConnected
      || mapped === SubmitResult.Terminated
    ? mapped
    : SubmitResult.Terminated;
}

function finiteDeadline(started: number, timeoutMs: number): number {
  return timeoutMs < 0 ? Number.POSITIVE_INFINITY : started + timeoutMs;
}

/**
 * Owns target-specific HWM admission for one DEALER or ROUTER. Core callbacks
 * only mark an exact target ready; all DONTWAIT attempts run later on the JS
 * event loop and release the complete-record attempt before waiting again.
 */
export class RoutedAdmission {
  private readonly native = requireNative();
  private readonly pendingByTarget = new Map<string, Set<PendingOperation>>();
  private readonly readyTargets: string[] = [];
  private readonly readySet = new Set<string>();
  private readonly lifecycle = new Set<PendingOperation>();
  private pumpScheduled = false;
  private pumping = false;
  private closed = false;

  constructor(
    private readonly handle: unknown,
    private readonly role: RoutedRole
  ) {
    this.native.socketRoutedSendReadyHandler(
      handle,
      (event: NativeReadyEvent) => this.onReady(event)
    );
  }

  send(
    selector: Buffer | null,
    payload: OperationPayloadValue<MessageLike>,
    sendTimeoutMs: number,
    startedAt: number,
    exactTarget: RoutedTarget | null = null
  ): Promise<void> {
    const snapshot = copiedPayload(payload);
    const operation: PendingOperation = {
      kind: 'send',
      selector,
      exactTarget,
      ...snapshot,
      deadline: finiteDeadline(startedAt, sendTimeoutMs),
      attemptBeforeExpiry: sendTimeoutMs === 0,
      target: null,
      key: null,
      phase: 'queued',
      attempted: false,
    };
    const promise = new Promise<void>((resolve, reject) => {
      operation.resolve = resolve;
      operation.reject = reject;
    });
    this.start(operation);
    return promise;
  }

  request(
    selector: Buffer | null,
    payload: OperationPayloadValue<MessageLike>,
    timeoutMs: number,
    startedAt: number,
    exactTarget: RoutedTarget | null = null
  ): Promise<Message[]> {
    const snapshot = copiedPayload(payload);
    if (this.closed) {
      return Promise.reject(submitFailure(
        SubmitResult.Terminated,
        SYNTHETIC_ERRNO,
        'routed operation submitted after socket close'
      ));
    }
    let request: NativeRequestRegistration;
    try {
      request = registerNativeRequest(this.handle, 'request failed');
    } catch (error) {
      return Promise.reject(error);
    }
    const operation: PendingOperation = {
      kind: 'request',
      selector,
      exactTarget,
      ...snapshot,
      deadline: finiteDeadline(startedAt, timeoutMs),
      attemptBeforeExpiry: false,
      request,
      target: null,
      key: null,
      phase: 'queued',
      attempted: false,
    };
    request.promise.then(
      () => this.requestCompleted(operation),
      () => this.requestCompleted(operation)
    );
    this.start(operation);
    return request.promise;
  }

  close(): void {
    if (this.closed) return;
    this.closed = true;
    this.native.socketRoutedAdmissionBeginClose(this.handle);
    const operations = [...this.lifecycle];
    for (const operation of operations) {
      const error = operation.kind === 'request' && operation.phase === 'accepted'
        ? withRuntimeErrorMessage(
            new RequestError(RequestResult.Terminated, SYNTHETIC_ERRNO),
            'request terminated because the socket closed'
          )
        : submitFailure(
            SubmitResult.Terminated,
            SYNTHETIC_ERRNO,
            'routed operation terminated because the socket closed'
          );
      this.fail(operation, error);
    }
    this.pendingByTarget.clear();
    this.readyTargets.length = 0;
    this.readySet.clear();
  }

  /** Called only after Core has finished closing the native socket. */
  finishClose(): void {
    this.native.socketRoutedAdmissionClose(this.handle);
    releaseNativeRequestDispatcher(this.handle);
  }

  private start(operation: PendingOperation): void {
    if (this.closed) {
      this.fail(operation, submitFailure(
        SubmitResult.Terminated,
        SYNTHETIC_ERRNO,
        'routed operation submitted after socket close'
      ));
      return;
    }
    this.lifecycle.add(operation);
    this.scheduleDeadline(operation);
    queueMicrotask(() => this.selectAndAttempt(operation));
  }

  private selectAndAttempt(operation: PendingOperation): void {
    if (!this.canRun(operation)) return;
    if (this.expired(operation) && !operation.attemptBeforeExpiry) {
      this.expire(operation);
      return;
    }

    try {
      const target = operation.exactTarget ?? this.selectTarget(operation.selector);
      operation.target = target;
      operation.key = targetKey(target);
      let pending = this.pendingByTarget.get(operation.key);
      if (!pending) {
        pending = new Set();
        this.pendingByTarget.set(operation.key, pending);
      }
      pending.add(operation);
      this.attempt(operation);
    } catch (error) {
      this.fail(operation, error);
    }
  }

  private selectTarget(selector: Buffer | null): RoutedTarget {
    const value = this.native.socketSelectRoutedSubmitTarget(
      this.handle,
      selector
    ) as NativeTargetResult;
    if (value.result !== SubmitResult.Ok
        || !value.peerRid
        || value.transportPairId === undefined
        || value.transportPairGeneration === undefined) {
      throw submitFailure(value.result, value.nativeErrno, 'routed target selection failed');
    }
    return {
      peerRid: value.peerRid,
      transportPairId: value.transportPairId,
      transportPairGeneration: value.transportPairGeneration,
    };
  }

  private attempt(operation: PendingOperation): void {
    if (!this.canRun(operation) || !operation.target || !operation.key) return;
    if (this.expired(operation)
        && !(operation.attemptBeforeExpiry && !operation.attempted)) {
      this.expire(operation);
      return;
    }

    operation.phase = 'attempting';
    operation.attempted = true;
    let result: NativeAttemptResult;
    try {
      result = operation.kind === 'send'
        ? this.sendAttempt(operation)
        : this.requestAttempt(operation);
    } catch (error) {
      this.fail(operation, submitNativeError(
        error,
        SendFlags.DontWait,
        `${operation.kind} failed`
      ));
      return;
    }
    this.finishAttempt(operation, result);
  }

  private sendAttempt(operation: PendingOperation): NativeAttemptResult {
    const target = operation.target!;
    return (this.role === 'dealer'
      ? this.native.dealerRoutedSendAttempt(
          this.handle,
          target.peerRid,
          target.transportPairId,
          target.transportPairGeneration,
          operation.nativeParts
        )
      : this.role === 'router'
        ? this.native.routerRoutedSendAttempt(
          this.handle,
          target.peerRid,
          target.transportPairId,
          target.transportPairGeneration,
          operation.nativeParts
        )
        : this.native.streamRoutedSendAttempt(
          this.handle,
          target.peerRid,
          target.transportPairId,
          target.transportPairGeneration,
          operation.nativeParts
        )) as NativeAttemptResult;
  }

  private requestAttempt(operation: PendingOperation): NativeAttemptResult {
    const target = operation.target!;
    const timeoutMs = this.remainingTimeout(operation.deadline);
    const token = operation.request!.token;
    return (this.role === 'dealer'
      ? this.native.dealerRoutedRequestAttempt(
          this.handle,
          target.peerRid,
          target.transportPairId,
          target.transportPairGeneration,
          operation.nativeParts,
          token,
          timeoutMs
        )
      : this.native.routerRoutedRequestAttempt(
          this.handle,
          target.peerRid,
          target.transportPairId,
          target.transportPairGeneration,
          operation.nativeParts,
          token,
          timeoutMs
        )) as NativeAttemptResult;
  }

  private finishAttempt(
    operation: PendingOperation,
    result: NativeAttemptResult
  ): void {
    if (operation.phase === 'terminal') return;
    if (this.closed) {
      this.fail(operation, submitFailure(
        SubmitResult.Terminated,
        result.nativeErrno,
        'routed operation terminated during socket close'
      ));
      return;
    }
    if (result.result === SubmitResult.Ok) {
      this.removePending(operation);
      this.clearDeadline(operation);
      consumeAccepted(operation.submittedMessages);
      if (operation.kind === 'send') {
        operation.phase = 'terminal';
        this.lifecycle.delete(operation);
        operation.resolve!();
      } else {
        operation.phase = 'accepted';
        operation.request!.activate();
      }
      return;
    }
    if (result.result === SubmitResult.Backpressured) {
      if (this.expired(operation)) {
        this.expire(operation);
      } else {
        operation.phase = 'waiting';
      }
      return;
    }
    this.fail(operation, submitFailure(
      result.result,
      result.nativeErrno,
      `${operation.kind} failed`
    ));
  }

  private onReady(event: NativeReadyEvent): void {
    if (this.closed) return;
    const key = targetKey(event);
    const pending = this.pendingByTarget.get(key);
    if (!pending || pending.size === 0) return;
    if (event.state === ROUTED_SEND_TERMINAL) {
      const result = terminalSubmitResult(event.terminalErrno);
      for (const operation of [...pending]) {
        this.fail(operation, submitFailure(
          result,
          event.terminalErrno,
          'routed target became terminal'
        ));
      }
      return;
    }
    if (event.state !== ROUTED_SEND_WRITABLE) return;
    if ((this.pendingByTarget.get(key)?.size ?? 0) > 0) {
      this.markReady(key);
    }
  }

  private markReady(key: string): void {
    if (this.readySet.has(key)) return;
    this.readySet.add(key);
    this.readyTargets.push(key);
    this.schedulePump();
  }

  private schedulePump(): void {
    if (this.closed || this.pumpScheduled || this.pumping) return;
    this.pumpScheduled = true;
    queueMicrotask(() => this.pump());
  }

  private pump(): void {
    if (this.closed || this.pumping) return;
    this.pumpScheduled = false;
    this.pumping = true;
    try {
      while (this.readyTargets.length > 0) {
        const key = this.readyTargets.shift()!;
        this.readySet.delete(key);
        const pending = this.pendingByTarget.get(key);
        if (!pending) continue;
        for (const operation of [...pending]) {
          if (operation.phase === 'waiting') this.attempt(operation);
        }
      }
    } finally {
      this.pumping = false;
      if (this.readyTargets.length > 0) this.schedulePump();
    }
  }

  private scheduleDeadline(operation: PendingOperation): void {
    if (!Number.isFinite(operation.deadline)) return;
    const delay = Math.max(0, operation.deadline - Date.now());
    operation.timer = setTimeout(() => this.expire(operation), delay);
    operation.timer.unref();
  }

  private clearDeadline(operation: PendingOperation): void {
    if (!operation.timer) return;
    clearTimeout(operation.timer);
    operation.timer = undefined;
  }

  private expire(operation: PendingOperation): void {
    if (operation.phase === 'terminal' || operation.phase === 'accepted') return;
    if (operation.attemptBeforeExpiry && !operation.attempted) {
      queueMicrotask(() => this.selectAndAttempt(operation));
      return;
    }
    this.fail(
      operation,
      operation.kind === 'request'
        ? requestTimeoutFailure()
        : submitFailure(
            SubmitResult.Backpressured,
            SYNTHETIC_ERRNO,
            'send timed out while waiting for routed admission'
          )
    );
  }

  private requestCompleted(operation: PendingOperation): void {
    if (operation.phase !== 'terminal') operation.phase = 'terminal';
    this.clearDeadline(operation);
    this.removePending(operation);
    this.lifecycle.delete(operation);
  }

  private fail(operation: PendingOperation, error: unknown): void {
    if (operation.phase === 'terminal') return;
    operation.phase = 'terminal';
    this.clearDeadline(operation);
    this.removePending(operation);
    this.lifecycle.delete(operation);
    if (operation.kind === 'request') {
      operation.request!.fail(error);
    } else {
      operation.reject!(error);
    }
  }

  private removePending(operation: PendingOperation): void {
    if (!operation.key) return;
    const pending = this.pendingByTarget.get(operation.key);
    pending?.delete(operation);
    if (pending?.size === 0) {
      this.pendingByTarget.delete(operation.key);
      this.readySet.delete(operation.key);
    }
  }

  private canRun(operation: PendingOperation): boolean {
    return operation.phase !== 'terminal' && !this.closed;
  }

  private expired(operation: PendingOperation): boolean {
    return Number.isFinite(operation.deadline)
      && Date.now() >= operation.deadline;
  }

  private remainingTimeout(deadline: number): number {
    if (!Number.isFinite(deadline)) return 0x7fffffff;
    return Math.max(1, Math.min(
      0x7fffffff,
      Math.ceil(deadline - Date.now())
    ));
  }
}
