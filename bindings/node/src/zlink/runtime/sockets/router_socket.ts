// SPDX-License-Identifier: MPL-2.0

import { RouterSocketOptions } from './socket_options';
import { normalizeOperationPayload } from '../buffers/message_conversion';
import { normalizeRoutingId } from '../core/routing_id';
import {
  ManagedRoutedRuntimeSendOperation,
  RuntimeReplyOperation,
  RuntimeRequestOperation,
  RoutedMessageSocket,
} from './socket_operations';
import { SendCompletionOwner, consumeSubmittedMessages } from '../messaging/send_completion';
import {
  registerNativeRequest,
  releaseNativeRequestDispatcher,
} from '../messaging/request_executor';
import { normalizeReplyFlags } from './socket_submit_errors';
import type { RuntimeContext as Context } from '../core/context';
import { configCall, submitNativeError } from '../errors/native_errors';
import { getNativeHandle } from '../handles/native_handle';
import { requireNative } from '../native/native';
import { RoutingId, type MessageLike } from '../../contracts';
import { SendFlags, SocketType as NativeSocketType } from '../../contracts/sockets/socket_constants';
import type {
  ReplyOperation,
  RequestOperation,
  RoutedSendOperation,
} from '../../contracts/messaging';
import { SubmitResult } from '../../contracts/errors/errors';
import { submitErrorFromNativeResult } from './socket_submit_errors';

const native = requireNative();

export class RouterSocket extends RoutedMessageSocket {
  private readonly sendCompletion: SendCompletionOwner;
  readonly options: RouterSocketOptions;
  constructor(ctx: Context) {
    super(ctx, NativeSocketType.ROUTER);
    this.options = RouterSocketOptions.create(this);
    try {
      this.sendCompletion = new SendCompletionOwner(getNativeHandle(this));
    } catch (error) {
      super.close();
      throw error;
    }
  }
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
  send(peerRid: RoutingId): RoutedSendOperation {
    const peer = normalizeRoutingId(peerRid, 'peerRid');
    return new ManagedRoutedRuntimeSendOperation(
      (selector, parts, timeoutMs) => this.sendCompletion.submit(parts, timeoutMs, selector),
      peer
    );
  }
  request(peerRid: RoutingId): RequestOperation {
    const peer = normalizeRoutingId(peerRid, 'peerRid');
    return new RuntimeRequestOperation((parts, timeoutMs) =>
      this.submitRequest(peer, parts, timeoutMs)
    );
  }
  requestTransportPair(
    peerRid: RoutingId,
    transportPairId: bigint,
    transportPairGeneration: bigint
  ): RequestOperation {
    const peer = normalizeRoutingId(peerRid, 'peerRid');
    const exactTarget = {
      peerRid: peer,
      transportPairId,
      transportPairGeneration,
    };
    return new RuntimeRequestOperation((parts, timeoutMs) =>
      this.submitRequest(
        peer,
        parts,
        timeoutMs,
        exactTarget.transportPairId,
        exactTarget.transportPairGeneration
      )
    );
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
  reply(peerRid: RoutingId, requestSeq: bigint): ReplyOperation {
    return new RuntimeReplyOperation((parts, opFlags) => this.replyDirect(peerRid, requestSeq, parts, opFlags));
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
  private submitRequest(
    peer: Buffer,
    parts: import('../messaging/send_operation_base').OperationPayloadValue<MessageLike>,
    timeoutMs: number,
    transportPairId = 0n,
    transportPairGeneration = 0n
  ): Promise<import('../../contracts').Message[]> {
    const handle = getNativeHandle(this);
    const registration = registerNativeRequest(handle, 'request failed');
    const resolvedTimeout = timeoutMs === 0
      ? (this.options.requestTimeout === 0 ? 5_000 : this.options.requestTimeout)
      : timeoutMs;
    let result: { result: number; nativeErrno: number };
    try {
      result = native.routerRequest(
        handle,
        peer,
        normalizeOperationPayload(parts),
        registration.token,
        resolvedTimeout,
        transportPairId,
        transportPairGeneration
      );
    } catch (error) {
      registration.fail(submitNativeError(error, SendFlags.DontWait, 'request submit failed'));
      return registration.promise;
    }
    if (result.result !== SubmitResult.Ok) {
      registration.fail(submitErrorFromNativeResult(
        result.result,
        result.nativeErrno,
        'request submit failed'
      ));
      return registration.promise;
    }
    consumeSubmittedMessages(parts);
    return registration.promise;
  }

  close(): void {
    const handle = getNativeHandle(this);
    super.close();
    releaseNativeRequestDispatcher(handle);
  }
}
