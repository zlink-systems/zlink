// SPDX-License-Identifier: MPL-2.0

import { StreamSocketOptions } from './socket_options';
import { SocketBase } from './socket_base';
import { messageFromNativeBuffer, normalizeMessageLikePayload } from '../buffers/message_conversion';
import { normalizeRoutingId } from '../core/routing_id';
import {
  RuntimeSendOperation,
} from './socket_operations';
import { submitErrorFromResult } from './socket_submit_errors';
import type { RuntimeContext as Context } from '../core/context';
import {
  configCall,
  handlerCall,
  recvNativeError,
  submitNativeError,
} from '../errors/native_errors';
import { getNativeHandle } from '../handles/native_handle';
import {
  materializeReceived,
  materializeReceivedInto,
} from '../messaging/message_materializer';
import { requireNative } from '../native/native';
import {
  Received,
  RoutingId,
  type Message,
  type MessageLike,
} from '../../contracts';
import { SubmitResult } from '../../contracts/errors/errors';
import { wrapRoutingId } from '../core/routing_id_conversion';
import { RecvFlags, SendFlags, SocketType as NativeSocketType } from '../../contracts/sockets/socket_constants';
import type {
  SendOperation,
  SocketSendReadyHandler,
  StreamPacketHandler,
} from '../../contracts/messaging';

const native = requireNative();

export class StreamSocket extends SocketBase {
  readonly options: StreamSocketOptions;
  private readonly _packetRoutingIdCache = new Map<string, RoutingId>();
  constructor(ctx: Context) {
    super(ctx, NativeSocketType.STREAM);
    this.options = StreamSocketOptions.create(this);
  }
  send(routingId: RoutingId): SendOperation {
    return new RuntimeSendOperation((parts, flags) => this.sendDirect(routingId, parts, flags));
  }
  private sendDirect(routingId: RoutingId, payload: MessageLike | readonly MessageLike[], flags: SendFlags = SendFlags.None): boolean {
    const normalized = normalizeMessageLikePayload(payload);
    const normalizedRoutingId = normalizeRoutingId(routingId);
    if ((flags | 0) & (SendFlags.DontWait | 0)) {
      let result;
      try {
        result = Array.isArray(normalized)
          ? native.socketSendRoutingNoWaitResultParts(getNativeHandle(this), normalizedRoutingId, normalized) as number
          : native.socketSendRoutingNoWaitResult(getNativeHandle(this), normalizedRoutingId, normalized) as number;
      } catch (error) {
        throw submitNativeError(error, flags, 'send failed');
      }
      if (result === SubmitResult.Ok) return true;
      if (result === SubmitResult.Backpressured) return false;
      throw submitErrorFromResult(result as SubmitResult, 'send failed');
    }
    try {
      if (Array.isArray(normalized)) {
        native.socketSendRoutingParts(
          getNativeHandle(this),
          normalizedRoutingId,
          normalized,
          flags | 0
        );
      } else {
        native.socketSendRouting(
          getNativeHandle(this),
          normalizedRoutingId,
          normalized,
          flags | 0
        );
      }
    } catch (error) {
      throw submitNativeError(error, flags, 'send failed');
    }
    return true;
  }
  recv(result: Received, flags?: RecvFlags): boolean;
  recv(resultOrFlags: Received | RecvFlags = RecvFlags.None,
      flags: RecvFlags = RecvFlags.None): Received | null | boolean {
    const result = resultOrFlags instanceof Received ? resultOrFlags : null;
    const recvFlags: RecvFlags = result ? flags : resultOrFlags as RecvFlags;
    let raw;
    try {
      raw = ((recvFlags | 0) & (RecvFlags.DontWait | 0))
        ? native.socketRecvMessageNoWait(getNativeHandle(this))
        : native.socketRecvMessage(getNativeHandle(this), recvFlags | 0);
    } catch (error) {
      throw recvNativeError(error, recvFlags, 'recv failed');
    }
    if (raw == null) return result ? false : null;
    const send = (parts: readonly Message[], sendFlags: SendFlags) => {
        if (!raw.routingId) {
          throw submitErrorFromResult(SubmitResult.InvalidState, 'missing routed send target');
        }
        return this.sendDirect(RoutingId.from(raw.routingId), parts, sendFlags);
      };
    if (!result) return materializeReceived(raw, undefined, send);
    materializeReceivedInto(result, raw, undefined, send);
    return true;
  }
  setPacketHandler(handler: StreamPacketHandler): void {
    handlerCall('stream packet handler registration failed', () => {
      native.socketStreamAttach(
        getNativeHandle(this),
        (routingId: Buffer | null, packets: Buffer[]) => {
          const sourceRid = this.packetRoutingId(routingId);
          if (!sourceRid) {
            return 0;
          }
          const header = messageFromNativeBuffer(packets[0]);
          const body = messageFromNativeBuffer(packets[1]);
          handler(sourceRid, header, body);
          return 0;
        },
        1
      );
    });
  }
  private packetRoutingId(routingId: Buffer | null): RoutingId | null {
    if (!routingId || routingId.length === 0) {
      return null;
    }
    // Hot path: STREAM callbacks receive the same peer routing ids repeatedly.
    // Cache the public RoutingId facade so packet dispatch does not allocate a
    // new wrapper for every frame while keeping the public handler contract.
    const key = routingId.toString('latin1');
    const cached = this._packetRoutingIdCache.get(key);
    if (cached) {
      return cached;
    }
    if (this._packetRoutingIdCache.size >= 16384) {
      this._packetRoutingIdCache.clear();
    }
    const wrapped = wrapRoutingId(routingId);
    if (wrapped) {
      this._packetRoutingIdCache.set(key, wrapped);
    }
    return wrapped;
  }
  setSendReadyHandler(handler: SocketSendReadyHandler): void {
    this.registerSendReadyHandler(handler);
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
  disconnectRid(routingId: RoutingId): void {
    const normalizedRoutingId = normalizeRoutingId(routingId);
    configCall('stream disconnect by routing id failed', () => {
      native.socketDisconnectRid(getNativeHandle(this), normalizedRoutingId);
    });
  }
}
