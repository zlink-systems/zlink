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
  registerNativeRequestCallback,
  messagesFromNativeBuffers,
  requestErrorFromResult,
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
      (selector, parts, flags) => {
        if (selector === null || !this.sendDirectRaw(selector, parts, flags)) {
          throw submitErrorFromNativeResult(
            selector === null ? SubmitResult.InvalidArgument : SubmitResult.Backpressured,
            0,
            'send failed'
          );
        }
      },
      peer
    );
  }
  request(peerRid: RoutingId): RequestOperation {
    const peer = normalizeRoutingId(peerRid, 'peerRid');
    return this.createRequestOperation(peer);
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
    return this.createRequestOperation(peer, exactTarget.transportPairId, exactTarget.transportPairGeneration);
  }
  private createRequestOperation(peer: Buffer, pairId = 0n, pairGeneration = 0n): RequestOperation {
    return new RuntimeRequestOperation(
      (parts, timeoutMs) => this.submitRequest(peer, parts, timeoutMs, pairId, pairGeneration),
      (parts, timeoutMs, flags) => this.submitRequestSync(peer, parts, timeoutMs, flags, pairId, pairGeneration),
      (parts, timeoutMs, flags, callback) => this.submitRequestCallback(peer, parts, timeoutMs, flags, callback, pairId, pairGeneration)
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
  protected sendReceivedAsync(
    routingId: Buffer,
    parts: readonly import('../../contracts').Message[],
    timeoutMs: number
  ): Promise<void> {
    return this.sendCompletion.submit(parts, timeoutMs, routingId);
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
        , SendFlags.DontWait
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
  private submitRequestSync(peer: Buffer, parts: any, timeoutMs: number, flags: SendFlags,
    pairId = 0n, pairGeneration = 0n): import('../../contracts').Message[] {
    const resolved = timeoutMs === 0 ? (this.options.requestTimeout === 0 ? 5_000 : this.options.requestTimeout) : timeoutMs;
    const result = native.routerRequestSync(getNativeHandle(this), peer, normalizeOperationPayload(parts), resolved,
      pairId, pairGeneration, flags | 0);
    if (result.result !== SubmitResult.Ok) throw submitErrorFromNativeResult(result.result, result.nativeErrno, 'request submit failed');
    consumeSubmittedMessages(parts);
    if (result.requestResult !== 0) throw requestErrorFromResult(result.requestResult as any, 'request failed');
    return messagesFromNativeBuffers(result.parts);
  }
  private submitRequestCallback(peer: Buffer, parts: any, timeoutMs: number, flags: SendFlags,
    callback: (error: Error | null, reply: import('../../contracts').Message[] | null) => void,
    pairId = 0n, pairGeneration = 0n): void {
    const handle = getNativeHandle(this);
    const registration = registerNativeRequestCallback(handle, callback, 'request failed');
    const resolved = timeoutMs === 0 ? (this.options.requestTimeout === 0 ? 5_000 : this.options.requestTimeout) : timeoutMs;
    let result;
    try { result = native.routerRequest(handle, peer, normalizeOperationPayload(parts), registration.token,
      resolved, pairId, pairGeneration, flags | 0, true); }
    catch (error) { registration.cancel(); throw submitNativeError(error, flags, 'request submit failed'); }
    if (result.result !== SubmitResult.Ok) {
      const error = submitErrorFromNativeResult(result.result, result.nativeErrno, 'request submit failed');
      registration.cancel(); throw error;
    }
    consumeSubmittedMessages(parts);
  }

  close(): void {
    const handle = getNativeHandle(this);
    super.close();
    releaseNativeRequestDispatcher(handle);
  }
}
