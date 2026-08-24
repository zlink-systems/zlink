// SPDX-License-Identifier: MPL-2.0

import { DealerSocketOptions } from './socket_options';
import { normalizeOperationPayload } from '../buffers/message_conversion';
import { normalizeRoutingId } from '../core/routing_id';
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
import { configCall, submitNativeError } from '../errors/native_errors';
import { getNativeHandle } from '../handles/native_handle';
import type { NativeReceivedRaw } from '../messaging/message_materializer';
import { requireNative } from '../native/native';
import { RoutingId } from '../../contracts';
import { RecvFlags, SendFlags, SocketType as NativeSocketType } from '../../contracts/sockets/socket_constants';
import type { RequestOperation, RoutedSendOperation } from '../../contracts/messaging';
import { SubmitResult } from '../../contracts/errors/errors';
import { submitErrorFromNativeResult } from './socket_submit_errors';

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
  close(): void {
    const handle = getNativeHandle(this);
    super.close();
    releaseNativeRequestDispatcher(handle);
  }
}
