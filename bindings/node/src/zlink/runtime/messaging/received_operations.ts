// SPDX-License-Identifier: MPL-2.0

import type { BufferLike } from '../../contracts/core/buffer_like';
import {
  Message,
  consumeSubmittedMessage,
} from '../../contracts/messaging/message';
import type {
  ReplyOperation,
  ReplySubmitOperation,
  RoutedSendOperation,
  RoutedSendSubmitOperation,
} from '../../contracts/messaging/operations';
import { SendFlags } from '../../contracts/sockets/socket_constants';
import { SendOperationBase } from './send_operation_base';

class RuntimeReceivedSendOperation
  extends SendOperationBase<Message | BufferLike, Message>
  implements RoutedSendOperation, RoutedSendSubmitOperation {
  private _timeoutMs = 0;

  constructor(
    private readonly _invoke: (parts: readonly Message[], flags: SendFlags) => void,
    private readonly _invokeAsync: (parts: readonly Message[], timeoutMs: number) => Promise<void>
  ) {
    super((message) => message instanceof Message ? message : Message.from(message));
  }

  timeout(timeoutMs: number): this {
    this.ensureOpen();
    if (!Number.isInteger(timeoutMs) || timeoutMs < -1 || timeoutMs > 0x7fffffff) {
      throw new RangeError('timeoutMs must be in the range -1..2147483647');
    }
    this._timeoutMs = timeoutMs;
    return this;
  }

  submit(): Promise<void> {
    return this._invokeAsync(this.consumeParts(), this._timeoutMs);
  }

  submit_sync(flags: SendFlags): void {
    const parts = this.consumeParts();
    this._invoke(parts, flags);
    for (const part of parts) consumeSubmittedMessage(part);
  }
}

class RuntimeReceivedReplyOperation
  extends SendOperationBase<Message | BufferLike, Message | BufferLike>
  implements ReplyOperation, ReplySubmitOperation {
  private readonly _invoke: (parts: readonly (Message | BufferLike)[], flags: SendFlags) => void;

  constructor(invoke: (parts: readonly (Message | BufferLike)[], flags: SendFlags) => void) {
    super((message) => message);
    this._invoke = invoke;
  }

  submit(): void {
    const parts = this.consumeParts();
    this._invoke(parts, this._flags);
    for (const part of parts) {
      if (part instanceof Message) consumeSubmittedMessage(part);
    }
  }
}

export function createReceivedSendOperation(
  invoke: (parts: readonly Message[], flags: SendFlags) => void,
  invokeAsync: (parts: readonly Message[], timeoutMs: number) => Promise<void>
): RoutedSendOperation {
  return new RuntimeReceivedSendOperation(invoke, invokeAsync);
}

export function createReceivedReplyOperation(
  invoke: (parts: readonly (Message | BufferLike)[], flags: SendFlags) => void
): ReplyOperation {
  return new RuntimeReceivedReplyOperation(invoke);
}
