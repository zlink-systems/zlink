// SPDX-License-Identifier: MPL-2.0

import { ReplyToken, RoutingId, type MessageLike } from '../../contracts';
import type { ReplyOperation, RequestOperation, SendOperation } from '../../contracts/messaging';
import {
  replyTokenNativeValue,
  replyTokenOwnerMatches,
} from '../../contracts/messaging/received';
import { SocketType as NativeSocketType } from '../../contracts/sockets/socket_constants';
import { normalizeOperationPayload } from '../buffers/message_conversion';
import type { RuntimeContext as Context } from '../core/context';
import { normalizeRoutingId } from '../core/routing_id';
import { configCall, submitNativeError } from '../errors/native_errors';
import { getNativeHandle } from '../handles/native_handle';
import { completionOwnerOf, consumeSubmittedMessages } from '../messaging/completion_owner';
import { requireNative } from '../native/native';
import {
  RoutedMessageSocket,
  RuntimeReplyOperation,
  RuntimeRequestOperation,
  RuntimeSendOperation,
} from './socket_operations';
import { RouterSocketOptions } from './socket_options';

const native = requireNative();

export class RouterSocket extends RoutedMessageSocket {
  readonly options: RouterSocketOptions;

  constructor(ctx: Context) {
    super(ctx, NativeSocketType.ROUTER);
    this.options = RouterSocketOptions.create(this);
  }

  setRoutingId(routingId: RoutingId): void {
    const normalized = normalizeRoutingId(routingId);
    configCall('routing id set failed', () =>
      native.handleSetRoutingId(getNativeHandle(this), normalized));
  }

  getRoutingId(): RoutingId {
    return RoutingId.from(configCall('routing id get failed', () =>
      native.handleGetRoutingId(getNativeHandle(this)) as Buffer));
  }

  send(peerRid: RoutingId): SendOperation {
    const target = Buffer.from(normalizeRoutingId(peerRid, 'peerRid'));
    return new RuntimeSendOperation(
      (parts) => completionOwnerOf(this).submitSend(parts, target),
      (parts) => completionOwnerOf(this).sendSync(parts, target)
    );
  }

  request(peerRid: RoutingId): RequestOperation {
    const target = Buffer.from(normalizeRoutingId(peerRid, 'peerRid'));
    return new RuntimeRequestOperation(
      (parts, timeoutMs) => completionOwnerOf(this).submitRequest(
        parts, target, this.resolveRequestTimeout(timeoutMs)),
      (parts, timeoutMs) => completionOwnerOf(this).requestSync(
        parts, target, this.resolveRequestTimeout(timeoutMs))
    );
  }

  reply(peerRid: RoutingId, token: ReplyToken): ReplyOperation {
    const target = Buffer.from(normalizeRoutingId(peerRid, 'peerRid'));
    this.validateReplyToken(token);
    return new RuntimeReplyOperation((parts) => this.replyDirect(target, token, parts));
  }

  protected replyToRoutedMessage(
    sourceRid: RoutingId,
    token: ReplyToken,
    parts: readonly MessageLike[]
  ): void {
    this.replyDirect(normalizeRoutingId(sourceRid), token, parts);
  }

  protected sendReceivedManaged(
    routingId: Buffer,
    parts: readonly import('../../contracts').Message[]
  ): Promise<void> {
    return completionOwnerOf(this).submitSend(parts, routingId);
  }

  private replyDirect(
    sourceRid: Buffer,
    token: ReplyToken,
    payload: MessageLike | readonly MessageLike[]
  ): void {
    this.validateReplyToken(token);
    try {
      native.socketReply(
        getNativeHandle(this),
        sourceRid,
        replyTokenNativeValue(token),
        normalizeOperationPayload(payload)
      );
    } catch (error) {
      throw submitNativeError(error, 0, 'reply failed');
    }
    consumeSubmittedMessages(payload);
  }

  private validateReplyToken(token: ReplyToken): void {
    if (!(token instanceof ReplyToken) || !replyTokenOwnerMatches(token, this.replyOwner)) {
      throw new TypeError('ReplyToken belongs to a different RouterSocket');
    }
  }

  private resolveRequestTimeout(timeoutMs: number): number {
    return timeoutMs === 0
      ? (this.options.requestTimeout === 0 ? 5_000 : this.options.requestTimeout)
      : timeoutMs;
  }
}
