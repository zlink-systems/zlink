// SPDX-License-Identifier: MPL-2.0

import type { BufferLike } from '../../contracts/core/buffer_like';
import { Message } from '../../contracts/messaging/message';
import type {
  ReplyOperation,
  ReplySubmitOperation,
  SendOperation,
  SendSubmitOperation,
} from '../../contracts/messaging/operations';
import { SendFlags } from '../../contracts/sockets/socket_constants';
import { SendOperationBase } from './send_operation_base';

class RuntimeReceivedSendOperation
  extends SendOperationBase<Message | BufferLike, Message>
  implements SendOperation, SendSubmitOperation {
  private readonly _invoke: (parts: readonly Message[], flags: SendFlags) => boolean;

  constructor(invoke: (parts: readonly Message[], flags: SendFlags) => boolean) {
    super((message) => message instanceof Message ? message : Message.from(message));
    this._invoke = invoke;
  }

  submit(): boolean {
    return this._invoke(this.consumeParts(), this._flags);
  }
}

class RuntimeReceivedReplyOperation
  extends SendOperationBase<Message | BufferLike, Message>
  implements ReplyOperation, ReplySubmitOperation {
  private readonly _invoke: (parts: readonly Message[], flags: SendFlags) => void;

  constructor(invoke: (parts: readonly Message[], flags: SendFlags) => void) {
    super((message) => message instanceof Message ? message : Message.from(message));
    this._invoke = invoke;
  }

  submit(): void {
    this._invoke(this.consumeParts(), this._flags);
  }
}

export function createReceivedSendOperation(
  invoke: (parts: readonly Message[], flags: SendFlags) => boolean
): SendOperation {
  return new RuntimeReceivedSendOperation(invoke);
}

export function createReceivedReplyOperation(
  invoke: (parts: readonly Message[], flags: SendFlags) => void
): ReplyOperation {
  return new RuntimeReceivedReplyOperation(invoke);
}
