// SPDX-License-Identifier: MPL-2.0

import { Message, type MessageLike } from '../../contracts';
import { consumeSubmittedMessage } from '../../contracts/messaging/message';
import { SendFlags } from '../../contracts/sockets/socket_constants';
import {
  PartOperationBase,
  SendOperationBase,
  type OperationPayloadValue,
} from '../messaging/send_operation_base';
import type {
  AsyncSendOperation,
  AsyncSendSubmitOperation,
  RequestOperation,
  RequestSubmitOperation,
  ReplyOperation,
  ReplySubmitOperation,
  RoutedSendOperation,
  RoutedSendSubmitOperation,
  SendOperation,
  SendSubmitOperation,
} from '../../contracts/messaging';

export type SendInvoker = (parts: OperationPayloadValue<MessageLike>, flags: SendFlags) => boolean;
export type PublishInvoker = (
  topic: string,
  payload: MessageLike | readonly MessageLike[],
  flags: SendFlags
) => boolean;
export type AsyncPublishInvoker = (
  topic: string,
  parts: OperationPayloadValue<MessageLike>,
  timeoutMs: number,
  startedAt: number
) => Promise<void>;
export type RequestInvoker = (
  parts: OperationPayloadValue<MessageLike>,
  timeoutMs: number,
  startedAt: number
) => Promise<Message[]>;
export type ReplyInvoker = (parts: OperationPayloadValue<MessageLike>, flags: SendFlags) => void;
export type ImmediateRoutedSendInvoker = (
  routingId: Buffer,
  parts: OperationPayloadValue<MessageLike>,
  flags: SendFlags
) => boolean;
export type ManagedRoutedSendInvoker = (
  routingId: Buffer | null,
  parts: OperationPayloadValue<MessageLike>,
  timeoutMs: number,
  startedAt: number
) => Promise<void>;

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
export class ImmediateRoutedRuntimeSendOperation
  extends SendOperationBase<MessageLike, MessageLike>
  implements SendOperation, SendSubmitOperation {
  private readonly _invoke: ImmediateRoutedSendInvoker;
  private readonly _routingId: Buffer;

  constructor(invoke: ImmediateRoutedSendInvoker, routingId: Buffer) {
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

/** @internal Managed DEALER/ROUTER builder with no callback or flags terminal. */
export class ManagedRoutedRuntimeSendOperation
  extends PartOperationBase<MessageLike, MessageLike>
  implements RoutedSendOperation, RoutedSendSubmitOperation {
  private readonly _invoke: ManagedRoutedSendInvoker;
  private readonly _routingId: Buffer | null;
  private _timeoutMs = 0;

  constructor(invoke: ManagedRoutedSendInvoker, routingId: Buffer | null) {
    super();
    this._invoke = invoke;
    this._routingId = routingId;
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
    const startedAt = Date.now();
    return this._invoke(this._routingId, this.consumePayload(), this._timeoutMs, startedAt);
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

export class AsyncPublishOperation
  extends PartOperationBase<MessageLike, MessageLike>
  implements AsyncSendOperation, AsyncSendSubmitOperation {
  private _timeoutMs = 0;

  constructor(
    private readonly _invoke: AsyncPublishInvoker,
    private readonly _topic: string
  ) { super(); }

  timeout(timeoutMs: number): this {
    this.ensureOpen();
    if (!Number.isInteger(timeoutMs) || timeoutMs < -1 || timeoutMs > 0x7fffffff) {
      throw new RangeError('timeoutMs must be in the range -1..2147483647');
    }
    this._timeoutMs = timeoutMs;
    return this;
  }

  submit(): Promise<void> {
    return this._invoke(this._topic, this.consumePayload(), this._timeoutMs, Date.now());
  }
}

export class RuntimeRequestOperation
  extends PartOperationBase<MessageLike, MessageLike>
  implements RequestOperation, RequestSubmitOperation {
  private readonly _invoke: RequestInvoker;
  private _timeoutMs = 0;

  constructor(invoke: RequestInvoker) {
    super();
    this._invoke = invoke;
  }

  timeout(timeoutMs: number): RequestSubmitOperation {
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
    const startedAt = Date.now();
    return this._invoke(this.consumePayload(), this._timeoutMs, startedAt);
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
