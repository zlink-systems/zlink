// SPDX-License-Identifier: MPL-2.0

import { RouterSocketOptions } from './socket_options';
import { messageFromNativeBuffer, normalizeOperationPayload } from '../buffers/message_conversion';
import { normalizeRoutingId } from '../core/routing_id';
import {
  RuntimeReplyOperation,
  RuntimeRequestOperation,
  RoutedMessageSocket,
} from './socket_operations';
import { normalizeReplyFlags } from './socket_submit_errors';
import type { RuntimeContext as Context } from '../core/context';
import { configCall, handlerCall, submitNativeError } from '../errors/native_errors';
import { getNativeHandle } from '../handles/native_handle';
import { executeNativeRequest } from '../messaging/request_executor';
import { startRequestProgress } from '../messaging/request_progress';
import { requireNative } from '../native/native';
import { RoutingId, type Message, type MessageLike } from '../../contracts';
import { SendFlags, SocketType as NativeSocketType } from '../../contracts/sockets/socket_constants';
import type {
  ReplyOperation,
  RequestCallback,
  RequestOperation,
} from '../../contracts/messaging';

const native = requireNative();

export class RouterSocket extends RoutedMessageSocket {
  private completionControlHandler?: (
    sourceRoutingId: RoutingId,
    parts: Message[]
  ) => void;
  private completionControlNativeRegistered = false;
  readonly options: RouterSocketOptions;
  constructor(ctx: Context) { super(ctx, NativeSocketType.ROUTER); this.options = RouterSocketOptions.create(this); }
  setRoutingId(routingId: RoutingId): void {
    const normalizedRoutingId = normalizeRoutingId(routingId);
    configCall('routing id set failed', () => {
      native.handleSetRoutingId(getNativeHandle(this), normalizedRoutingId);
    });
  }
  getRoutingId(): RoutingId {
    return RoutingId.from(
      configCall('routing id get failed', () =>
        native.handleGetRoutingId(getNativeHandle(this)) as Buffer
      )
    );
  }
  request(peerRid: RoutingId): RequestOperation {
    return new RuntimeRequestOperation((parts, cbOrTimeout, opFlags, opTimeout) =>
      this.requestDirect(peerRid, parts, cbOrTimeout, opFlags, opTimeout)
    );
  }
  requestTransportPair(
    peerRid: RoutingId,
    transportPairId: bigint,
    transportPairGeneration: bigint
  ): RequestOperation {
    return new RuntimeRequestOperation((parts, cbOrTimeout, opFlags, opTimeout) => {
      const peer = normalizeRoutingId(peerRid, 'peerRid');
      const nativeHandle = getNativeHandle(this);
      return executeNativeRequest({
        handle: nativeHandle,
        callbackOrTimeout: cbOrTimeout,
        flagsOrTimeout: opFlags,
        maybeTimeout: opTimeout,
        startProgress: () => startRequestProgress(nativeHandle),
        invoke: (token, flags, timeoutMs) => {
          native.routerRequestTransportPair(
            nativeHandle,
            peer,
            transportPairId,
            transportPairGeneration,
            parts,
            token,
            flags | 0,
            timeoutMs | 0
          );
        },
        submitErrorMessage: 'transport-pair request failed',
        requestErrorMessage: 'transport-pair request failed'
      });
    });
  }
  sendTransportPair(
    peerRid: RoutingId,
    transportPairId: bigint,
    transportPairGeneration: bigint,
    payloadOrParts: MessageLike | readonly MessageLike[],
    flags: SendFlags = SendFlags.None
  ): void {
    const peer = normalizeRoutingId(peerRid, 'peerRid');
    const parts = normalizeOperationPayload(payloadOrParts);
    try {
      native.routerSendTransportPair(
        getNativeHandle(this),
        peer,
        transportPairId,
        transportPairGeneration,
        parts,
        flags | 0
      );
    } catch (error) {
      throw submitNativeError(error, flags, 'transport-pair send failed');
    }
  }
  private requestDirect(
    peerRid: RoutingId,
    payloadOrParts: MessageLike | readonly MessageLike[],
    callbackOrTimeout?: RequestCallback | number,
    flagsOrTimeout?: SendFlags | number,
    maybeTimeout?: number,
  ): Promise<Message[]> | boolean {
    const parts = normalizeOperationPayload(payloadOrParts);
    const peer = normalizeRoutingId(peerRid, 'peerRid');
    const nativeHandle = getNativeHandle(this);
    return executeNativeRequest({
      handle: nativeHandle,
      callbackOrTimeout,
      flagsOrTimeout,
      maybeTimeout,
      startProgress: () => startRequestProgress(nativeHandle),
      invoke: (token, flags, timeoutMs) => {
        native.routerRequest(
          nativeHandle,
          peer,
          parts,
          token,
          flags | 0,
          timeoutMs | 0
        );
      },
      submitErrorMessage: 'request failed',
      requestErrorMessage: 'request failed'
    });
  }
  reply(peerRid: RoutingId, requestSeq: bigint): ReplyOperation {
    return new RuntimeReplyOperation((parts, opFlags) => this.replyDirect(peerRid, requestSeq, parts, opFlags));
  }
  trySendCompletionControl(
    peerRid: RoutingId,
    payloadOrParts: readonly MessageLike[]
  ): boolean {
    const peer = normalizeRoutingId(peerRid, 'peerRid');
    const parts = normalizeOperationPayload(payloadOrParts);
    try {
      return native.routerTrySendCompletionControl(
        getNativeHandle(this),
        peer,
        parts
      );
    } catch (error) {
      throw submitNativeError(error, SendFlags.None, 'completion control failed');
    }
  }
  trySendCompletionControlTransportPair(
    peerRid: RoutingId,
    transportPairId: bigint,
    transportPairGeneration: bigint,
    payloadOrParts: readonly MessageLike[]
  ): boolean {
    const peer = normalizeRoutingId(peerRid, 'peerRid');
    const parts = normalizeOperationPayload(payloadOrParts);
    try {
      return native.routerTrySendCompletionControlTransportPair(
        getNativeHandle(this),
        peer,
        transportPairId,
        transportPairGeneration,
        parts
      );
    } catch (error) {
      throw submitNativeError(error, SendFlags.None, 'transport-pair completion control failed');
    }
  }
  setCompletionControlHandler(
    handler: (sourceRoutingId: RoutingId, parts: Message[]) => void
  ): void {
    if (typeof handler !== 'function') {
      throw new TypeError('handler must be a function');
    }
    const previous = this.completionControlHandler;
    this.completionControlHandler = handler;
    if (this.completionControlNativeRegistered) return;

    try {
      handlerCall('completion control handler failed', () => {
        native.routerCompletionControlHandler(
          getNativeHandle(this),
          (sourceRoutingId, parts) => {
            const current = this.completionControlHandler;
            const messages = parts.map(messageFromNativeBuffer);
            if (current) {
              current(RoutingId.from(sourceRoutingId), messages);
              return;
            }
            for (const message of messages) message.close();
          }
        );
      });
      this.completionControlNativeRegistered = true;
    } catch (error) {
      this.completionControlHandler = previous;
      throw error;
    }
  }
  protected replyToRoutedMessage(
    sourceRid: RoutingId,
    requestSeq: bigint,
    parts: readonly MessageLike[],
    flags: SendFlags,
  ): void {
    this.replyDirect(sourceRid, requestSeq, parts, flags);
  }
  private replyDirect(peerRid: RoutingId, requestSeq: bigint, payloadOrParts: MessageLike | readonly MessageLike[], flags: SendFlags = SendFlags.None): void {
    normalizeReplyFlags(flags);
    const normalizedPeerRid = normalizeRoutingId(peerRid, 'peerRid');
    const parts = normalizeOperationPayload(payloadOrParts);
    try {
      native.routerReply(
        getNativeHandle(this),
        normalizedPeerRid,
        requestSeq,
        parts
      );
    } catch (error) {
      throw submitNativeError(error, flags, 'reply failed');
    }
  }
}
