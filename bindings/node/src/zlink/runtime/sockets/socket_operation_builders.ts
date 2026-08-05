// SPDX-License-Identifier: MPL-2.0

import { Message, type MessageLike } from '../../contracts';
import { SendFlags } from '../../contracts/sockets/socket_constants';
import type { OperationPayloadValue } from '../messaging/operation_payload';
import { SendOperationBase } from '../messaging/send_operation_base';
import type {
  RequestCallback,
  RequestCallbackSubmitOperation,
  RequestOperation,
  RequestSubmitOperation,
  ReplyOperation,
  ReplySubmitOperation,
  SendOperation,
  SendSubmitOperation,
} from '../../contracts/messaging';

export type SendInvoker = (parts: OperationPayloadValue<MessageLike>, flags: SendFlags) => boolean;
export type PublishInvoker = (
  topic: string,
  payload: MessageLike | readonly MessageLike[],
  flags: SendFlags
) => boolean;
export type RequestInvoker = (
  parts: OperationPayloadValue<MessageLike>,
  callbackOrTimeout?: RequestCallback | number,
  flagsOrTimeout?: SendFlags | number,
  maybeTimeout?: number
) => Promise<Message[]> | boolean;
export type ReplyInvoker = (parts: OperationPayloadValue<MessageLike>, flags: SendFlags) => void;

export class RuntimeSendOperation
  extends SendOperationBase<MessageLike, MessageLike>
  implements SendOperation, SendSubmitOperation {
  private readonly _invoke: SendInvoker;

  constructor(invoke: SendInvoker) {
    super((message) => message);
    this._invoke = invoke;
  }

  submit(): boolean {
    return this._invoke(this.consumePayload(), this._flags);
  }
}

export class PublishOperation
  extends SendOperationBase<MessageLike, MessageLike>
  implements SendOperation, SendSubmitOperation {
  private readonly _invoke: PublishInvoker;
  private readonly _topic: string;

  constructor(invoke: PublishInvoker, topic: string) {
    super((message) => message);
    this._invoke = invoke;
    this._topic = topic;
  }

  submit(): boolean {
    return this._invoke(this._topic, this.consumePayload(), this._flags);
  }
}

export class RuntimeRequestOperation
  extends SendOperationBase<MessageLike, MessageLike>
  implements RequestOperation, RequestSubmitOperation, RequestCallbackSubmitOperation {
  private readonly _invoke: RequestInvoker;
  private _timeoutMs = 0;
  private _callbackMode = false;

  constructor(invoke: RequestInvoker) {
    super((message) => message);
    this._invoke = invoke;
  }

  timeout(timeoutMs: number): RequestSubmitOperation {
    this.ensureOpen();
    this._timeoutMs = timeoutMs | 0;
    return this;
  }

  flags(flags: SendFlags): this {
    super.flags(flags);
    this._callbackMode = true;
    return this;
  }

  submit(): Promise<Message[]>;
  submit(callback: RequestCallback): boolean;
  submit(callback?: RequestCallback): Promise<Message[]> | boolean {
    if (callback === undefined) {
      return this._invoke(this.consumePayload(), this._timeoutMs) as Promise<Message[]>;
    }
    const flags = this._callbackMode ? this._flags : SendFlags.None;
    return this._invoke(this.consumePayload(), callback, flags, this._timeoutMs) as boolean;
  }
}

export class RuntimeReplyOperation
  extends SendOperationBase<MessageLike, MessageLike>
  implements ReplyOperation, ReplySubmitOperation {
  private readonly _invoke: ReplyInvoker;

  constructor(invoke: ReplyInvoker) {
    super((message) => message);
    this._invoke = invoke;
  }

  submit(): void {
    this._invoke(this.consumePayload(), this._flags);
  }
}
