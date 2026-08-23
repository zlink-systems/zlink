// SPDX-License-Identifier: MPL-2.0

import type { BufferLike } from '../../contracts/core/buffer_like';
import {
  Message,
  consumeSubmittedMessage,
} from '../../contracts/messaging/message';
import type {
  ReplyOperation,
  ReplySubmitOperation,
  ImmediateSendOperation,
  ImmediateSendSubmitOperation,
} from '../../contracts/messaging/operations';
import { SendFlags } from '../../contracts/sockets/socket_constants';
import { SendOperationBase } from './send_operation_base';

class RuntimeReceivedSendOperation
  extends SendOperationBase<Message | BufferLike, Message>
  implements ImmediateSendOperation, ImmediateSendSubmitOperation {
  private readonly _invoke: (parts: readonly Message[], flags: SendFlags) => boolean;

  constructor(invoke: (parts: readonly Message[], flags: SendFlags) => boolean) {
    super((message) => message instanceof Message ? message : Message.from(message));
    this._invoke = invoke;
  }

  submit(): boolean {
    const parts = this.consumeParts();
    const accepted = this._invoke(parts, this._flags);
    if (accepted) {
      for (const part of parts) consumeSubmittedMessage(part);
    }
    return accepted;
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
  invoke: (parts: readonly Message[], flags: SendFlags) => boolean
): ImmediateSendOperation {
  return new RuntimeReceivedSendOperation(invoke);
}

export function createReceivedReplyOperation(
  invoke: (parts: readonly (Message | BufferLike)[], flags: SendFlags) => void
): ReplyOperation {
  return new RuntimeReceivedReplyOperation(invoke);
}
