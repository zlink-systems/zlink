// SPDX-License-Identifier: MPL-2.0

import { requireNative } from '../native/native';
import { getNativeHandle } from '../handles/native_handle';
import {
  configCall,
  recvNativeError,
  submitOrBackpressure,
  submitNativeError
} from '../errors/native_errors';
import {
  adoptTopicMessage,
  materializeReceivedInto,
  materializeRoutedReceivedInto,
  materializeTopicMessage,
  routedReceivedRoutingBytes,
  routedReceivedPrefersManagedBuffer,
} from '../messaging/message_materializer';
import {
  normalizeOperationPayload,
} from '../buffers/message_conversion';
import { normalizeRoutingId, routingIdFromOwnedBuffer } from '../core/routing_id';
import { ConnectableSocket } from './socket_base';
import {
  Message,
  Received,
  RoutingId,
  TopicMessage,
  type MessageLike,
} from '../../contracts';
import { validateCString } from '../options/validation';
import {
  SendFlags,
  RecvFlags,
} from '../../contracts/sockets/socket_constants';
import {
  SubmitResult,
} from '../../contracts/errors/errors';

import type {
  SubscriptionEntry,
  SendOperation,
  PublishOperation as PublishOperationContract,
} from '../../contracts/messaging';
import {
  PublishOperation,
  RuntimeSendOperation,
} from './socket_operation_builders';
import { completionOwnerOf } from '../messaging/completion_owner';
export {
  PublishOperation,
  RuntimeReplyOperation,
  RuntimeRequestOperation,
  RuntimeSendOperation,
} from './socket_operation_builders';
import { submitErrorFromResult } from './socket_submit_errors';

const native = requireNative();

export class ReceiveSocket extends ConnectableSocket {
  /**
   * Receives into caller-provided storage. Pass a long-lived {@link Received}
   * and the binding refills its internal state in place each successful call.
   * Returns true on success and false when DontWait finds no data.
   */
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
}

export class SendSocket extends ReceiveSocket {
  send(): SendOperation {
    return new RuntimeSendOperation(
      (payload) => completionOwnerOf(this).submitSend(payload, null),
      (payload) => completionOwnerOf(this).sendSync(payload, null)
    );
  }
}

export class PublisherSocket extends ConnectableSocket {
  private lastPublishTopic: string | undefined;
  private lastValidatedPublishTopic: string | undefined;
  private readonly publishInvoker =
    (topic: string, payload: MessageLike | readonly MessageLike[]) =>
      this.publishDirect(topic, payload);

  constructor(ctx: import('../core/context').RuntimeContext, type: number) {
    super(ctx, type);
  }

  publish(topic: string): PublishOperationContract {
    const normalizedTopic = topic === this.lastPublishTopic
      ? this.lastValidatedPublishTopic!
      : validateCString(topic, 'topic', Number.MAX_SAFE_INTEGER);
    this.lastPublishTopic = topic;
    this.lastValidatedPublishTopic = normalizedTopic;
    return new PublishOperation(
      this.publishInvoker,
      normalizedTopic
    );
  }
  /** @internal */
  publishDirect(topic: string, payload: MessageLike | readonly MessageLike[], flags: SendFlags = SendFlags.None): boolean {
    const normalized = normalizeOperationPayload(payload);
    if ((flags | 0) & (SendFlags.DontWait | 0)) {
      let result;
      try {
        result = native.socketTryPublish(
          getNativeHandle(this),
          topic,
          normalized
        ) as number;
      } catch (error) {
        throw submitNativeError(error, flags, 'publish failed');
      }
      if (result === SubmitResult.Ok) return true;
      if (result === SubmitResult.Backpressured) return false;
      throw submitErrorFromResult(result as SubmitResult, 'publish failed');
    }
    try {
      native.socketPublish(getNativeHandle(this), topic, normalized, flags | 0);
      return true;
    } catch (error) {
      return submitOrBackpressure(error, flags, 'publish failed');
    }
  }
}

export class SubscriberSocket extends ConnectableSocket {
  setSubscription(topicOrPattern: string): void {
    const topic = validateCString(topicOrPattern, 'topicOrPattern', Number.MAX_SAFE_INTEGER);
    configCall('subscription set failed', () => {
      native.socketSetSubscription(getNativeHandle(this), topic);
    });
  }
  unsetSubscription(topicOrPattern: string): void {
    const topic = validateCString(topicOrPattern, 'topicOrPattern', Number.MAX_SAFE_INTEGER);
    configCall('subscription unset failed', () => {
      native.socketUnsetSubscription(getNativeHandle(this), topic);
    });
  }
  subscriptionAt(index: number): SubscriptionEntry | null {
    return configCall('subscription lookup failed', () =>
      native.subscriptionAt(getNativeHandle(this), index >>> 0) as SubscriptionEntry | null
    );
  }
  subscribe(result: TopicMessage, flags?: RecvFlags): boolean;
  subscribe(resultOrFlags: TopicMessage | RecvFlags = RecvFlags.None,
            maybeFlags: RecvFlags = RecvFlags.None): TopicMessage | null | boolean {
    const hasResult = resultOrFlags instanceof TopicMessage;
    const flags = hasResult ? maybeFlags : resultOrFlags as RecvFlags;
    let raw;
    try {
      raw = ((flags | 0) & (RecvFlags.DontWait | 0))
        ? native.socketTrySubscribeMessage(getNativeHandle(this))
        : native.socketSubscribeMessage(getNativeHandle(this), flags | 0);
    } catch (error) {
      throw recvNativeError(error, flags, 'subscribe failed');
    }
    if (!raw) {
      return hasResult ? false : null;
    }
    if (hasResult) {
      adoptTopicMessage(resultOrFlags, raw);
      return true;
    }
    return materializeTopicMessage(raw);
  }

}

export class RoutedMessageSocket extends ConnectableSocket {
  protected readonly replyOwner = {};
  private readonly receivedOperations = {
    replyOwner: this.replyOwner,
    send: (routingId: Buffer, parts: readonly Message[]) =>
      completionOwnerOf(this).sendSync(parts, routingId),
    sendManaged: (routingId: Buffer, parts: readonly Message[]) =>
      this.sendReceivedManaged(routingId, parts),
    reply: (routingId: Buffer, token: import('../../contracts').ReplyToken,
            parts: readonly Message[]) =>
      this.replyToRoutedMessage(routingIdFromOwnedBuffer(routingId), token, parts),
  };
  protected sendDirect(routingId: RoutingId, payload: MessageLike | readonly MessageLike[]): void {
    const normalizedRoutingId = normalizeRoutingId(routingId);
    completionOwnerOf(this).sendSync(payload, normalizedRoutingId);
  }
  protected replyToRoutedMessage(
    _sourceRid: RoutingId,
    _replyToken: import('../../contracts').ReplyToken,
    _parts: readonly Message[],
  ): void {
    throw submitErrorFromResult(
      SubmitResult.InvalidState,
      'request reply is only supported by RouterSocket'
    );
  }
  protected sendReceivedManaged(
    _routingId: Buffer,
    _parts: readonly Message[],
  ): Promise<void> {
    return Promise.reject(submitErrorFromResult(
      SubmitResult.InvalidState,
      'managed received send is not supported by this socket'
    ));
  }

  /**
   * Receives into caller-provided storage. See {@link MessageSocket.recv}.
   */
  recv(result: Received, flags: RecvFlags = RecvFlags.None): boolean {
    // HOT PATH: terminal readers repeatedly materialize data(), whereas
    // relays consume the movable native frame. Use the previous refill to
    // select the next internal storage mode without changing the public API.
    const preferManagedSinglePart = routedReceivedPrefersManagedBuffer(result);
    const routingIdStorage = routedReceivedRoutingBytes(result);
    let raw;
    try {
      raw = ((flags | 0) & (RecvFlags.DontWait | 0))
        ? native.routerRecvMessageNoWait(
            getNativeHandle(this),
            preferManagedSinglePart,
            routingIdStorage
          )
        : native.routerRecvMessage(
            getNativeHandle(this),
            flags | 0,
            preferManagedSinglePart,
            routingIdStorage
          );
    } catch (error) {
      throw recvNativeError(error, flags, 'recv failed');
    }
    if (raw == null) return false;
    materializeRoutedReceivedInto(result, raw, this.receivedOperations);
    return true;
  }


}
