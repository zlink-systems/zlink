// SPDX-License-Identifier: MPL-2.0

import type { BufferLike } from '../../contracts/core/buffer_like';
import {
  Message,
  consumeSubmittedMessage,
} from '../../contracts/messaging/message';
import type {
  ReplyOperation,
  ReplySubmitOperation,
  SendOperation,
  SendSubmitOperation,
} from '../../contracts/messaging/operations';
import { PartOperationBase } from './send_operation_base';

class RuntimeReceivedSendOperation
  extends PartOperationBase<Message | BufferLike, Message>
  implements SendOperation, SendSubmitOperation {

  constructor(
    private readonly _invoke: (parts: readonly Message[]) => void,
    private readonly _invokeAsync: (parts: readonly Message[]) => Promise<void>
  ) {
    super((message) => message instanceof Message ? message : Message.from(message));
  }

  submit(): Promise<void> {
    return this._invokeAsync(this.consumeParts());
  }

  submit_sync(): void {
    const parts = this.consumeParts();
    this._invoke(parts);
    for (const part of parts) consumeSubmittedMessage(part);
  }
}

class RuntimeReceivedReplyOperation
  extends PartOperationBase<Message | BufferLike, Message | BufferLike>
  implements ReplyOperation, ReplySubmitOperation {
  private readonly _invoke: (parts: readonly (Message | BufferLike)[]) => void;

  constructor(invoke: (parts: readonly (Message | BufferLike)[]) => void) {
    super((message) => message);
    this._invoke = invoke;
  }

  submit(): void {
    const parts = this.consumeParts();
    this._invoke(parts);
    for (const part of parts) {
      if (part instanceof Message) consumeSubmittedMessage(part);
    }
  }
}

export function createReceivedSendOperation(
  invoke: (parts: readonly Message[]) => void,
  invokeAsync: (parts: readonly Message[]) => Promise<void>
): SendOperation {
  return new RuntimeReceivedSendOperation(invoke, invokeAsync);
}

export function createReceivedReplyOperation(
  invoke: (parts: readonly (Message | BufferLike)[]) => void
): ReplyOperation {
  return new RuntimeReceivedReplyOperation(invoke);
}
