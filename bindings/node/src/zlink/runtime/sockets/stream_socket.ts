// SPDX-License-Identifier: MPL-2.0

import {
  Received,
  RoutingId,
  StreamPacket,
  type Message,
  type SendOperation,
} from '../../contracts';
import { streamPacketState } from '../../contracts/messaging/stream_packet';
import { RecvFlags, SocketType as NativeSocketType } from '../../contracts/sockets/socket_constants';
import { messageFromNativeBuffer } from '../buffers/message_conversion';
import type { RuntimeContext as Context } from '../core/context';
import { normalizeRoutingId } from '../core/routing_id';
import { configCall, recvNativeError } from '../errors/native_errors';
import { getNativeHandle } from '../handles/native_handle';
import { completionOwnerOf } from '../messaging/completion_owner';
import {
  materializeReceivedInto,
  nativeReceivedRoutingId,
} from '../messaging/message_materializer';
import { requireNative } from '../native/native';
import { SocketBase } from './socket_base';
import { RuntimeSendOperation } from './socket_operations';
import { StreamSocketOptions } from './socket_options';

const native = requireNative();

export class StreamSocket extends SocketBase {
  readonly options: StreamSocketOptions;

  constructor(ctx: Context) {
    super(ctx, NativeSocketType.STREAM);
    this.options = StreamSocketOptions.create(this);
  }

  send(routingId: RoutingId): SendOperation {
    const target = Buffer.from(normalizeRoutingId(routingId));
    return new RuntimeSendOperation(
      (parts) => completionOwnerOf(this).submitSend(parts, target),
      (parts) => completionOwnerOf(this).sendSync(parts, target)
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
    const routingId = nativeReceivedRoutingId(raw);
    const sendSync = (parts: readonly Message[]): void => {
      if (!routingId) throw new Error('missing routed send target');
      completionOwnerOf(this).sendSync(parts, routingId);
    };
    const sendManaged = (parts: readonly Message[]): Promise<void> => {
      if (!routingId) return Promise.reject(new Error('missing routed send target'));
      return completionOwnerOf(this).submitSend(parts, routingId);
    };
    materializeReceivedInto(result, raw, sendSync, sendManaged);
    return true;
  }

  recvPacket(result: StreamPacket, flags: RecvFlags = RecvFlags.None): boolean {
    if (!(result instanceof StreamPacket)) {
      throw new TypeError('result must be a StreamPacket');
    }
    const state = streamPacketState(result);
    if (state._receiving) throw new Error('StreamPacket is already in use by recvPacket');
    result.close();
    state._receiving = true;
    try {
      const raw = native.socketStreamRecvPacket(getNativeHandle(this), flags | 0);
      if (raw == null) return false;
      state._routingId = RoutingId.from(raw.routingId);
      state._header = messageFromNativeBuffer(raw.header);
      state._body = messageFromNativeBuffer(raw.body);
      return true;
    } catch (error) {
      result.close();
      throw recvNativeError(error, flags, 'stream packet recv failed');
    } finally {
      state._receiving = false;
    }
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

  disconnectRid(routingId: RoutingId): void {
    const normalized = normalizeRoutingId(routingId);
    configCall('stream disconnect by routing id failed', () =>
      native.socketDisconnectRid(getNativeHandle(this), normalized));
  }
}
