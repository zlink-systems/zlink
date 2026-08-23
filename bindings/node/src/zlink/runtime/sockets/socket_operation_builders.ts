// SPDX-License-Identifier: MPL-2.0

import { Message, type MessageLike } from '../../contracts';
import { consumeSubmittedMessage } from '../../contracts/messaging/message';
import { SendFlags } from '../../contracts/sockets/socket_constants';
import type {
  ImmediateSendOperation,
  ImmediateSendSubmitOperation,
  PublishOperation as PublishOperationContract,
  PublishSubmitOperation,
  RequestOperation,
  RequestSubmitOperation,
  ReplyOperation,
  ReplySubmitOperation,
  RoutedSendOperation,
  RoutedSendSubmitOperation,
  SendOperation,
  SendSubmitOperation,
} from '../../contracts/messaging';
import {
  PartOperationBase,
  SendOperationBase,
  type OperationPayloadValue,
} from '../messaging/send_operation_base';

export type ManagedSendInvoker = (
  parts: OperationPayloadValue<MessageLike>,
  timeoutMs: number
) => Promise<void>;
export type PublishInvoker = (
  topic: string,
  payload: OperationPayloadValue<MessageLike>
) => void;
export type RequestInvoker = (
  parts: OperationPayloadValue<MessageLike>,
  timeoutMs: number
) => Promise<Message[]>;
export type ReplyInvoker = (parts: OperationPayloadValue<MessageLike>, flags: SendFlags) => void;
export type ImmediateSendInvoker = (
  routingId: Buffer,
  parts: OperationPayloadValue<MessageLike>,
  flags: SendFlags
) => boolean;
export type ManagedRoutedSendInvoker = (
  routingId: Buffer | null,
  parts: OperationPayloadValue<MessageLike>,
  timeoutMs: number
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

function validateSendTimeout(timeoutMs: number): number {
  if (!Number.isInteger(timeoutMs) || timeoutMs < -1 || timeoutMs > 0x7fffffff) {
    throw new RangeError('timeoutMs must be in the range -1..2147483647');
  }
  return timeoutMs;
}

/** Core-completion-driven PAIR send builder. */
export class RuntimeSendOperation
  extends PartOperationBase<MessageLike, MessageLike>
  implements SendOperation, SendSubmitOperation {
  private _timeoutMs = 0;

  constructor(private readonly _invoke: ManagedSendInvoker) { super(); }

  timeout(timeoutMs: number): this {
    this.ensureOpen();
    this._timeoutMs = validateSendTimeout(timeoutMs);
    return this;
  }

  submit(): Promise<void> {
    return this._invoke(this.consumePayload(), this._timeoutMs);
  }
}

/** Immediate raw routed builder retained for STREAM trySend/relay paths. */
export class ImmediateRoutedRuntimeSendOperation
  extends SendOperationBase<MessageLike, MessageLike>
  implements ImmediateSendOperation, ImmediateSendSubmitOperation {
  private readonly _routingId: Buffer;

  constructor(
    private readonly _invoke: ImmediateSendInvoker,
    routingId: Buffer
  ) {
    super();
    this._routingId = routingId;
  }

  submit(): boolean {
    const payload = this.consumePayload();
    const accepted = this._invoke(this._routingId, payload, this._flags);
    if (accepted) consumeSubmittedMessages(payload);
    return accepted;
  }
}

/** Core-completion-driven DEALER/ROUTER/STREAM routed send builder. */
export class ManagedRoutedRuntimeSendOperation
  extends PartOperationBase<MessageLike, MessageLike>
  implements RoutedSendOperation, RoutedSendSubmitOperation {
  private _timeoutMs = 0;

  constructor(
    private readonly _invoke: ManagedRoutedSendInvoker,
    private readonly _routingId: Buffer | null
  ) { super(); }

  timeout(timeoutMs: number): this {
    this.ensureOpen();
    this._timeoutMs = validateSendTimeout(timeoutMs);
    return this;
  }

  submit(): Promise<void> {
    return this._invoke(this._routingId, this.consumePayload(), this._timeoutMs);
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

  constructor(private readonly _invoke: RequestInvoker) { super(); }

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
}

export class RuntimeReplyOperation
  extends SendOperationBase<MessageLike, MessageLike>
  implements ReplyOperation, ReplySubmitOperation {
  constructor(private readonly _invoke: ReplyInvoker) { super(); }

  submit(): void {
    const payload = this.consumePayload();
    this._invoke(payload, this._flags);
    consumeSubmittedMessages(payload);
  }
}
