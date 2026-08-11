// SPDX-License-Identifier: MPL-2.0

import { Message, type MessageLike } from '../../contracts';
import { consumeSubmittedMessage } from '../../contracts/messaging/message';
import { SendFlags } from '../../contracts/sockets/socket_constants';
import {
  SendOperationBase,
  type OperationPayloadValue,
} from '../messaging/send_operation_base';
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
export type RoutedSendInvoker = (
  routingId: Buffer,
  parts: OperationPayloadValue<MessageLike>,
  flags: SendFlags
) => boolean;

function consumeSubmittedMessages(payload: OperationPayloadValue<MessageLike>): void {
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

export class RuntimeSendOperation
  extends SendOperationBase<MessageLike, MessageLike>
  implements SendOperation, SendSubmitOperation {
  private readonly _invoke: SendInvoker;

  constructor(invoke: SendInvoker) {
    super();
    this._invoke = invoke;
  }

  submit(): boolean {
    const payload = this.consumePayload();
    const accepted = this._invoke(payload, this._flags);
    if (accepted) consumeSubmittedMessages(payload);
    return accepted;
  }
}

/** @internal A routed builder with route bytes and socket submitter fixed at construction. */
export class RoutedRuntimeSendOperation
  extends SendOperationBase<MessageLike, MessageLike>
  implements SendOperation, SendSubmitOperation {
  private readonly _invoke: RoutedSendInvoker;
  private readonly _routingId: Buffer;

  constructor(invoke: RoutedSendInvoker, routingId: Buffer) {
    super();
    this._invoke = invoke;
    this._routingId = routingId;
  }

  submit(): boolean {
    const payload = this.consumePayload();
    const accepted = this._invoke(this._routingId, payload, this._flags);
    if (accepted) consumeSubmittedMessages(payload);
    return accepted;
  }
}

export class PublishOperation
  extends SendOperationBase<MessageLike, MessageLike>
  implements SendOperation, SendSubmitOperation {
  private readonly _invoke: PublishInvoker;
  private readonly _topic: string;

  constructor(invoke: PublishInvoker, topic: string) {
    super();
    this._invoke = invoke;
    this._topic = topic;
  }

  submit(): boolean {
    const payload = this.consumePayload();
    const accepted = this._invoke(this._topic, payload, this._flags);
    if (accepted) consumeSubmittedMessages(payload);
    return accepted;
  }
}

export class RuntimeRequestOperation
  extends SendOperationBase<MessageLike, MessageLike>
  implements RequestOperation, RequestSubmitOperation, RequestCallbackSubmitOperation {
  private readonly _invoke: RequestInvoker;
  private _timeoutMs = 0;
  private _callbackMode = false;

  constructor(invoke: RequestInvoker) {
    super();
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
    const payload = this.consumePayload();
    if (callback === undefined) {
      const reply = this._invoke(payload, this._timeoutMs) as Promise<Message[]>;
      consumeSubmittedMessages(payload);
      return reply;
    }
    const flags = this._callbackMode ? this._flags : SendFlags.None;
    const accepted = this._invoke(payload, callback, flags, this._timeoutMs) as boolean;
    if (accepted) consumeSubmittedMessages(payload);
    return accepted;
  }
}

export class RuntimeReplyOperation
  extends SendOperationBase<MessageLike, MessageLike>
  implements ReplyOperation, ReplySubmitOperation {
  private readonly _invoke: ReplyInvoker;

  constructor(invoke: ReplyInvoker) {
    super();
    this._invoke = invoke;
  }

  submit(): void {
    const payload = this.consumePayload();
    this._invoke(payload, this._flags);
    consumeSubmittedMessages(payload);
  }
}
