// SPDX-License-Identifier: MPL-2.0

import { PubSocketOptions } from './socket_options';
import {
  PublisherSocket,
} from './socket_operations';
import type { RuntimeContext as Context } from '../core/context';
import { recvNativeError } from '../errors/native_errors';
import { getNativeHandle } from '../handles/native_handle';
import { requireNative } from '../native/native';
import { SubscriptionEvent } from '../../contracts';
import { wrapRoutingId } from '../core/routing_id_conversion';
import {
  createSubscriptionEvent,
  replaceSubscriptionEvent
} from '../messaging/subscription_event_state';
import { RecvFlags, SocketType as NativeSocketType } from '../../contracts/sockets/socket_constants';

export class XPubSocket extends PublisherSocket {
  readonly options: PubSocketOptions;
  constructor(ctx: Context) { super(ctx, NativeSocketType.XPUB); this.options = PubSocketOptions.create(this); }
  receiveSubscriptionEvent(result: SubscriptionEvent, flags?: RecvFlags): boolean;
  receiveSubscriptionEvent(resultOrFlags: SubscriptionEvent | RecvFlags = RecvFlags.None,
                           maybeFlags: RecvFlags = RecvFlags.None): SubscriptionEvent | null | boolean {
    const hasResult = resultOrFlags instanceof SubscriptionEvent;
    const flags = hasResult ? maybeFlags : resultOrFlags as RecvFlags;
    let raw;
    try {
      raw = ((flags | 0) & (RecvFlags.DontWait | 0))
        ? requireNative().socketTrySubscriptionEvent(getNativeHandle(this)) as { routingId?: Buffer | null; topic: string; subscribed: boolean } | null
        : requireNative().socketSubscriptionEvent(getNativeHandle(this), flags | 0) as { routingId?: Buffer | null; topic: string; subscribed: boolean } | null;
    } catch (error) {
      throw recvNativeError(error, flags, 'subscription event recv failed');
    }
    if (!raw) {
      return hasResult ? false : null;
    }
    if (hasResult) {
      replaceSubscriptionEvent(
        resultOrFlags,
        raw.topic,
        raw.subscribed,
        wrapRoutingId(raw.routingId ?? null)
      );
      return true;
    }
    return createSubscriptionEvent(raw.topic, raw.subscribed, wrapRoutingId(raw.routingId ?? null));
  }
}
