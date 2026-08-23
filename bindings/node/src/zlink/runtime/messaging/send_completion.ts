// SPDX-License-Identifier: MPL-2.0

import { Message, type MessageLike } from '../../contracts';
import { consumeSubmittedMessage } from '../../contracts/messaging/message';
import { SubmitError, SubmitResult } from '../../contracts/errors/errors';
import { normalizeOperationPayload } from '../buffers/message_conversion';
import { submitNativeError } from '../errors/native_errors';
import { withRuntimeErrorMessage } from '../errors/error_state';
import { requireNative } from '../native/native';
import type { OperationPayloadValue } from './send_operation_base';

const SEND_ADMITTED = 0;
const SEND_TIMED_OUT = 201;
const SEND_TERMINAL = 202;

// zlink_errno.h keeps these extended values stable across platforms. The
// system ECANCELED value remains 125 on the supported Node platforms.
const ZLINK_HAUSNUMERO = 156384712;
const ETIMEDOUT = ZLINK_HAUSNUMERO + 16;
const EHOSTUNREACH = ZLINK_HAUSNUMERO + 17;
const ESHUTDOWN = ZLINK_HAUSNUMERO + 22;
const ETERM = ZLINK_HAUSNUMERO + 53;
const ECANCELED = 125;

interface NativeSendCompletionEvent {
  readonly token: bigint;
  readonly opId: bigint;
  readonly result: number;
  readonly terminalErrno: number;
  readonly peerRid: Buffer;
  readonly transportPairId: bigint;
  readonly transportPairGeneration: bigint;
}

interface NativeSendSubmitResult {
  readonly result: number;
  readonly nativeErrno: number;
  readonly opId: bigint;
  readonly inlineCompletion?: NativeSendCompletionEvent;
}

interface PendingSend {
  readonly resolve: () => void;
  readonly reject: (error: unknown) => void;
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

function terminalResult(nativeErrno: number): SubmitResult {
  if (nativeErrno === ECANCELED || nativeErrno === ESHUTDOWN || nativeErrno === ETERM) {
    return SubmitResult.Terminated;
  }
  if (nativeErrno === 2 || nativeErrno === 113 || nativeErrno === EHOSTUNREACH) {
    return SubmitResult.NotFound;
  }
  return SubmitResult.NotConnected;
}

export function consumeSubmittedMessages(payload: OperationPayloadValue<MessageLike>): void {
  if (payload instanceof Message) {
    consumeSubmittedMessage(payload);
    return;
  }
  if (Array.isArray(payload)) {
    for (const part of payload) {
      if (part instanceof Message) consumeSubmittedMessage(part);
    }
  }
}

/**
 * Correlates Core send completions with the Promise returned by submit().
 * Core owns all admitted message records; this class owns only Promise state.
 */
export class SendCompletionOwner {
  private readonly native = requireNative();
  private readonly pending = new Map<bigint, PendingSend>();
  private nextToken = 1n;

  constructor(private readonly handle: unknown) {
    this.native.socketSendCompletionHandler(
      handle,
      (event: NativeSendCompletionEvent) => this.onCompletion(event)
    );
  }

  submit(
    payload: OperationPayloadValue<MessageLike>,
    timeoutMs: number,
    routingId: Buffer | null
  ): Promise<void> {
    const token = this.nextToken++;
    let resolvePromise!: () => void;
    let rejectPromise!: (error: unknown) => void;
    const promise = new Promise<void>((resolve, reject) => {
      resolvePromise = resolve;
      rejectPromise = reject;
    });
    this.pending.set(token, { resolve: resolvePromise, reject: rejectPromise });

    let submitted: NativeSendSubmitResult;
    try {
      submitted = this.native.socketSendAsync(
        this.handle,
        normalizeOperationPayload(payload),
        timeoutMs > 0 ? timeoutMs : 0,
        routingId,
        token
      ) as NativeSendSubmitResult;
    } catch (error) {
      this.pending.delete(token);
      rejectPromise(submitNativeError(error, 0, 'send submit failed'));
      return promise;
    }

    if (submitted.result !== SubmitResult.Ok) {
      this.pending.delete(token);
      rejectPromise(submitFailure(
        submitted.result,
        submitted.nativeErrno,
        'send submit failed'
      ));
      return promise;
    }

    // zlink_send_async has transferred every part to Core at this point,
    // including an inline completion that has not yet returned to JavaScript.
    consumeSubmittedMessages(payload);
    if (submitted.inlineCompletion) this.onCompletion(submitted.inlineCompletion);
    return promise;
  }

  private onCompletion(event: NativeSendCompletionEvent): void {
    const operation = this.pending.get(event.token);
    if (!operation) return;
    this.pending.delete(event.token);

    if (event.result === SEND_ADMITTED) {
      operation.resolve();
      return;
    }
    if (event.result === SEND_TIMED_OUT) {
      operation.reject(submitFailure(
        SubmitResult.Backpressured,
        event.terminalErrno || ETIMEDOUT,
        'send timed out while waiting for Core admission'
      ));
      return;
    }
    if (event.result === SEND_TERMINAL) {
      const errno = event.terminalErrno;
      operation.reject(submitFailure(
        terminalResult(errno),
        errno,
        'send terminated before Core admission'
      ));
      return;
    }
    operation.reject(submitFailure(
      SubmitResult.InternalError,
      event.terminalErrno,
      'send completion returned an unknown result'
    ));
  }
}
