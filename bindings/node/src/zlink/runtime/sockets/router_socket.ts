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
import { RoutedAdmission, resolvedRequestTimeout } from './routed_admission';
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

const native = requireNative();

export class RouterSocket extends RoutedMessageSocket {
  private readonly admission: RoutedAdmission;
  readonly options: RouterSocketOptions;
  constructor(ctx: Context) {
    super(ctx, NativeSocketType.ROUTER);
    this.options = RouterSocketOptions.create(this);
    try {
      this.admission = new RoutedAdmission(getNativeHandle(this), 'router');
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
      (selector, parts, timeoutMs, startedAt) => this.admission.send(
        selector,
        parts,
        timeoutMs === 0 ? this.options.sendTimeout : timeoutMs,
        startedAt
      ),
      peer
    );
  }
  request(peerRid: RoutingId): RequestOperation {
    const peer = normalizeRoutingId(peerRid, 'peerRid');
    return new RuntimeRequestOperation((parts, timeoutMs, startedAt) =>
      this.admission.request(
        peer,
        parts,
        resolvedRequestTimeout(timeoutMs, this.options.requestTimeout),
        startedAt
      )
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
    return new RuntimeRequestOperation((parts, timeoutMs, startedAt) =>
      this.admission.request(
        peer,
        parts,
        resolvedRequestTimeout(timeoutMs, this.options.requestTimeout),
        startedAt,
        exactTarget
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
  close(): void {
    this.admission.close();
    super.close();
    this.admission.finishClose();
  }
}
