// SPDX-License-Identifier: MPL-2.0

import { DealerSocketOptions } from './socket_options';
import { normalizeOperationPayload } from '../buffers/message_conversion';
import { normalizeRoutingId } from '../core/routing_id';
import { materializeReceivedInto } from '../messaging/message_materializer';
import {
  ManagedRoutedRuntimeSendOperation,
  ReceiveSocket,
  RuntimeRequestOperation,
} from './socket_operations';
import { SendCompletionOwner, consumeSubmittedMessages } from '../messaging/send_completion';
import {
  registerNativeRequest,
  releaseNativeRequestDispatcher,
} from '../messaging/request_executor';
import type { RuntimeContext as Context } from '../core/context';
import { configCall, recvNativeError, submitNativeError } from '../errors/native_errors';
import { getNativeHandle } from '../handles/native_handle';
import { requireNative } from '../native/native';
import { RoutingId } from '../../contracts';
import { RecvFlags, SendFlags, SocketType as NativeSocketType } from '../../contracts/sockets/socket_constants';
import type { MessageLike, Received, RequestOperation, RoutedSendOperation } from '../../contracts/messaging';
import { SubmitResult } from '../../contracts/errors/errors';
import { normalizeReplyFlags, submitErrorFromNativeResult } from './socket_submit_errors';

const native = requireNative();

export class DealerSocket extends ReceiveSocket {
  private readonly sendCompletion: SendCompletionOwner;
  readonly options: DealerSocketOptions;
  constructor(ctx: Context) {
    super(ctx, NativeSocketType.DEALER);
    this.options = DealerSocketOptions.create(this);
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
  send(): RoutedSendOperation {
    return new ManagedRoutedRuntimeSendOperation(
      (_selector, parts, timeoutMs) => this.sendCompletion.submit(parts, timeoutMs, null),
      null
    );
  }
  request(): RequestOperation {
    return new RuntimeRequestOperation((parts, timeoutMs) => {
      const handle = getNativeHandle(this);
      const registration = registerNativeRequest(handle, 'request failed');
      const resolvedTimeout = timeoutMs === 0
        ? (this.options.requestTimeout === 0 ? 5_000 : this.options.requestTimeout)
        : timeoutMs;
      let result: { result: number; nativeErrno: number };
      try {
        result = native.dealerRequest(
          handle,
          normalizeOperationPayload(parts),
          registration.token,
          resolvedTimeout
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
    });
  }
  recv(result: Received, flags: RecvFlags = RecvFlags.None): boolean {
    let raw;
    try {
      raw = ((flags | 0) & (RecvFlags.DontWait | 0))
        ? native.dealerRecvMessageNoWait(getNativeHandle(this))
        : native.dealerRecvMessage(getNativeHandle(this), flags | 0);
    } catch (error) {
      throw recvNativeError(error, flags, 'recv failed');
    }
    if (raw == null) return false;
    materializeReceivedInto(
      result,
      raw,
      (requestSeq, parts, replyFlags) => this.replyDirect(requestSeq, parts, replyFlags)
    );
    return true;
  }
  close(): void {
    const handle = getNativeHandle(this);
    super.close();
    releaseNativeRequestDispatcher(handle);
  }
  private replyDirect(
    requestSeq: bigint,
    payloadOrParts: MessageLike | readonly MessageLike[],
    flags: SendFlags = SendFlags.None
  ): void {
    normalizeReplyFlags(flags);
    try {
      native.dealerReply(
        getNativeHandle(this),
        requestSeq,
        normalizeOperationPayload(payloadOrParts)
      );
    } catch (error) {
      throw submitNativeError(error, flags, 'reply failed');
    }
  }
}
