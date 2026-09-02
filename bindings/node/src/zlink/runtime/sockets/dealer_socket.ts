// SPDX-License-Identifier: MPL-2.0

import { RoutingId } from '../../contracts';
import type { Received, RequestOperation, SendOperation } from '../../contracts/messaging';
import { RecvFlags, SocketType as NativeSocketType } from '../../contracts/sockets/socket_constants';
import { normalizeRoutingId } from '../core/routing_id';
import type { RuntimeContext as Context } from '../core/context';
import { configCall, recvNativeError } from '../errors/native_errors';
import { getNativeHandle } from '../handles/native_handle';
import { completionOwnerOf } from '../messaging/completion_owner';
import { materializeReceivedInto } from '../messaging/message_materializer';
import { requireNative } from '../native/native';
import { ReceiveSocket, RuntimeRequestOperation, RuntimeSendOperation } from './socket_operations';
import { DealerSocketOptions } from './socket_options';

const native = requireNative();

export class DealerSocket extends ReceiveSocket {
  readonly options: DealerSocketOptions;

  constructor(ctx: Context) {
    super(ctx, NativeSocketType.DEALER);
    this.options = DealerSocketOptions.create(this);
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

  send(): SendOperation {
    return new RuntimeSendOperation(
      (parts) => completionOwnerOf(this).submitSend(parts, null),
      (parts) => completionOwnerOf(this).sendSync(parts, null)
    );
  }

  request(): RequestOperation {
    return new RuntimeRequestOperation(
      (parts, timeoutMs) => completionOwnerOf(this).submitRequest(
        parts, null, this.resolveRequestTimeout(timeoutMs)),
      (parts, timeoutMs) => completionOwnerOf(this).requestSync(
        parts, null, this.resolveRequestTimeout(timeoutMs))
    );
  }

  recv(result: Received, flags: RecvFlags = RecvFlags.None): boolean {
    let raw;
    try {
      raw = ((flags | 0) & (RecvFlags.DontWait | 0))
        ? native.socketRecvMessageNoWait(getNativeHandle(this))
        : native.socketRecvMessage(getNativeHandle(this), flags | 0);
    } catch (error) {
      throw recvNativeError(error, flags, 'recv failed');
    }
    if (raw == null) return false;
    materializeReceivedInto(result, raw);
    return true;
  }

  private resolveRequestTimeout(timeoutMs: number): number {
    return timeoutMs === 0
      ? (this.options.requestTimeout === 0 ? 5_000 : this.options.requestTimeout)
      : timeoutMs;
  }
}
