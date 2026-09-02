// SPDX-License-Identifier: MPL-2.0

import { Message, type MessageLike } from '../../contracts';
import { consumeSubmittedMessage } from '../../contracts/messaging/message';
import type {
  PublishOperation as PublishOperationContract,
  PublishSubmitOperation,
  RequestOperation,
  RequestSubmitOperation,
  ReplyOperation,
  ReplySubmitOperation,
  SendOperation,
  SendSubmitOperation,
} from '../../contracts/messaging';
import {
  PartOperationBase,
  type OperationPayloadValue,
} from '../messaging/send_operation_base';

export type ManagedSendInvoker = (
  parts: OperationPayloadValue<MessageLike>
) => Promise<void>;
export type SyncSendInvoker = (
  parts: OperationPayloadValue<MessageLike>
) => void;
export type PublishInvoker = (
  topic: string,
  payload: OperationPayloadValue<MessageLike>
) => void;
export type RequestInvoker = (
  parts: OperationPayloadValue<MessageLike>,
  timeoutMs: number
) => Promise<Message[]>;
export type SyncRequestInvoker = (
  parts: OperationPayloadValue<MessageLike>, timeoutMs: number
) => Message[];
export type ReplyInvoker = (parts: OperationPayloadValue<MessageLike>) => void;

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

/** Core-completion-driven PAIR send builder. */
export class RuntimeSendOperation
  extends PartOperationBase<MessageLike, MessageLike>
  implements SendOperation, SendSubmitOperation {
  constructor(
    private readonly _invoke: ManagedSendInvoker,
    private readonly _invokeSync: SyncSendInvoker
  ) { super(); }

  submit(): Promise<void> { return this._invoke(this.consumePayload()); }
  submit_sync(): void {
    const payload = this.consumePayload();
    this._invokeSync(payload);
    consumeSubmittedMessages(payload);
  }
}

/** Synchronous PUB/XPUB builder; Core decides drop versus NODROP error. */
export class PublishOperation
  extends PartOperationBase<MessageLike, MessageLike>
  implements PublishOperationContract, PublishSubmitOperation {
  constructor(
    private readonly _invoke: PublishInvoker,
    private readonly _topic: string
  ) { super(); }

  submit(): void {
    const payload = this.consumePayload();
    this._invoke(this._topic, payload);
    consumeSubmittedMessages(payload);
  }
}

export class RuntimeRequestOperation
  extends PartOperationBase<MessageLike, MessageLike>
  implements RequestOperation, RequestSubmitOperation {
  private _timeoutMs = 0;

  constructor(
    private readonly _invoke: RequestInvoker,
    private readonly _invokeSync: SyncRequestInvoker
  ) { super(); }

  timeout(timeoutMs: number): this {
    this.ensureOpen();
    if (!Number.isInteger(timeoutMs)) {
      throw new TypeError('timeoutMs must be an integer');
    }
    if (timeoutMs < 0 || timeoutMs > 0x7fffffff) {
      throw new RangeError('timeoutMs must be in the range 0..2147483647');
    }
    this._timeoutMs = timeoutMs;
    return this;
  }

  submit(): Promise<Message[]> {
    return this._invoke(this.consumePayload(), this._timeoutMs);
  }

  submit_sync(): Message[] {
    return this._invokeSync(this.consumePayload(), this._timeoutMs);
  }
}

export class RuntimeReplyOperation
  extends PartOperationBase<MessageLike, MessageLike>
  implements ReplyOperation, ReplySubmitOperation {
  constructor(private readonly _invoke: ReplyInvoker) { super(); }

  submit(): void {
    const payload = this.consumePayload();
    this._invoke(payload);
    consumeSubmittedMessages(payload);
  }
}
