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
  materializeTopicMessage,
  type NativeReceivedRaw,
  type NativeTopicMessageRaw,
} from '../messaging/message_materializer';
import {
  normalizeMessageLikePayload,
  normalizeOperationPayload,
} from '../buffers/message_conversion';
import { normalizeRoutingId } from '../core/routing_id';
import { ConnectableSocket, SendReadySocket } from './socket_base';
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
} from '../../contracts/messaging';
import {
  PublishOperation,
  RuntimeSendOperation,
} from './socket_operation_builders';
export {
  PublishOperation,
  RuntimeReplyOperation,
  RuntimeRequestOperation,
  RuntimeSendOperation,
} from './socket_operation_builders';
import { submitErrorFromResult } from './socket_submit_errors';
import { setBufferedReceive } from './socket_receive_state';
import { BufferedReceiveQueue } from './buffered_receive_queue';

const native = requireNative();
const RECV_BATCH_MESSAGE_LIMIT = 16;

export class SendSocket extends SendReadySocket {
  send(): SendOperation {
    return new RuntimeSendOperation((parts, flags) => this.sendDirect(parts, flags));
  }
  protected sendDirect(payloadOrParts: MessageLike | readonly MessageLike[], flags: SendFlags = SendFlags.None): boolean {
    const payload = normalizeMessageLikePayload(payloadOrParts);
    if ((flags | 0) & (SendFlags.DontWait | 0)) {
      let result;
      try {
        result = Array.isArray(payload)
          ? native.socketSendNoWaitResultParts(getNativeHandle(this), payload) as number
          : native.socketSendNoWaitResult(getNativeHandle(this), payload) as number;
      } catch (error) {
        throw submitNativeError(error, flags, 'send failed');
      }
      if (result === SubmitResult.Ok) return true;
      if (result === SubmitResult.Backpressured) return false;
      throw submitErrorFromResult(result as SubmitResult, 'send failed');
    }
    try {
      if (Array.isArray(payload)) {
        native.socketSendParts(getNativeHandle(this), payload, flags | 0);
      } else {
        native.socketSend(getNativeHandle(this), payload, flags | 0);
      }
      return true;
    } catch (error) {
      throw submitNativeError(error, flags, 'send failed');
    }
  }
}

export class PublisherSocket extends SendReadySocket {
  publish(topic: string): SendOperation {
    return new PublishOperation(
      (normalizedTopic, payload, flags) => this.publishDirect(normalizedTopic, payload, flags),
      validateCString(topic, 'topic', Number.MAX_SAFE_INTEGER)
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

export class MessageSocket extends SendSocket {
  private readonly bufferedReceive =
    new BufferedReceiveQueue<NativeReceivedRaw>(this);

  /**
   * Receives into caller-provided storage. Pass a long-lived {@link Received}
   * and the binding refills its internal state in place each successful call.
   * Returns true on success and false when DontWait finds no data.
   */
  recv(result: Received, flags: RecvFlags = RecvFlags.None): boolean {
    let raw;
    try {
      raw = this.bufferedReceive.take();
      if (raw == null && ((flags | 0) & (RecvFlags.DontWait | 0))) {
        this.bufferedReceive.replace(native.socketRecvMessageBatchNoWait(
          getNativeHandle(this),
          RECV_BATCH_MESSAGE_LIMIT
        ));
        raw = this.bufferedReceive.take();
      } else if (raw == null) {
        raw = native.socketRecvMessage(getNativeHandle(this), flags | 0);
      }
    } catch (error) {
      throw recvNativeError(error, flags, 'recv failed');
    }
    if (raw == null) {
      setBufferedReceive(this, false);
      return false;
    }
    materializeReceivedInto(result, raw);
    return true;
  }

  close(): void {
    this.bufferedReceive.drain((raw) => {
      const pending = new Received();
      materializeReceivedInto(pending, raw);
      pending.close();
    });
    super.close();
  }
}

export class SubscriberSocket extends ConnectableSocket {
  private readonly bufferedReceive =
    new BufferedReceiveQueue<NativeTopicMessageRaw>(this);

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
      raw = this.bufferedReceive.take();
      if (raw == null && ((flags | 0) & (RecvFlags.DontWait | 0))) {
        this.bufferedReceive.replace(native.socketTrySubscribeMessageBatch(
          getNativeHandle(this),
          RECV_BATCH_MESSAGE_LIMIT
        ));
        raw = this.bufferedReceive.take();
      } else if (raw == null) {
        raw = native.socketSubscribeMessage(getNativeHandle(this), flags | 0);
      }
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

  close(): void {
    this.bufferedReceive.drain((raw) => materializeTopicMessage(raw).close());
    super.close();
  }
}

export class RoutedMessageSocket extends SendReadySocket {
  private readonly bufferedReceive =
    new BufferedReceiveQueue<NativeReceivedRaw>(this);

  send(routingId: RoutingId): SendOperation {
    return new RuntimeSendOperation((parts, flags) => this.sendDirect(routingId, parts, flags));
  }
  protected sendDirect(routingId: RoutingId, payload: MessageLike | readonly MessageLike[], flags: SendFlags = SendFlags.None): boolean {
    const normalizedRoutingId = normalizeRoutingId(routingId);
    return this.sendDirectRaw(normalizedRoutingId, payload, flags);
  }
  protected sendDirectRaw(routingId: Buffer, payload: MessageLike | readonly MessageLike[], flags: SendFlags = SendFlags.None): boolean {
    const normalized = normalizeMessageLikePayload(payload);
    if ((flags | 0) & (SendFlags.DontWait | 0)) {
      let result;
      try {
        result = Array.isArray(normalized)
          ? native.socketSendRoutingNoWaitResultParts(getNativeHandle(this), routingId, normalized) as number
          : native.socketSendRoutingNoWaitResult(getNativeHandle(this), routingId, normalized) as number;
      } catch (error) {
        throw submitNativeError(error, flags, 'send failed');
      }
      if (result === SubmitResult.Ok) return true;
      if (result === SubmitResult.Backpressured) return false;
      throw submitErrorFromResult(result as SubmitResult, 'send failed');
    }
    if (!Array.isArray(normalized)) {
      try {
        native.socketSendRouting(getNativeHandle(this), routingId, normalized, flags | 0);
      } catch (error) {
        throw submitNativeError(error, flags, 'send failed');
      }
      return true;
    }
    try {
      native.socketSendRoutingParts(
        getNativeHandle(this),
        routingId,
        normalized,
        flags | 0
      );
    } catch (error) {
      throw submitNativeError(error, flags, 'send failed');
    }
    return true;
  }
  protected replyToRoutedMessage(
    _sourceRid: RoutingId,
    _requestSeq: bigint,
    _parts: readonly Message[],
    _flags: SendFlags,
  ): void {
    throw submitErrorFromResult(
      SubmitResult.InvalidState,
      'request reply is only supported by RouterSocket'
    );
  }

  /**
   * Receives into caller-provided storage. See {@link MessageSocket.recv}.
   */
  recv(result: Received, flags: RecvFlags = RecvFlags.None): boolean {
    let raw;
    try {
      raw = this.bufferedReceive.take();
      if (raw == null && ((flags | 0) & (RecvFlags.DontWait | 0))) {
        this.bufferedReceive.replace(native.routerRecvMessageBatchNoWait(
          getNativeHandle(this),
          RECV_BATCH_MESSAGE_LIMIT
        ));
        raw = this.bufferedReceive.take();
      } else if (raw == null) {
        raw = native.routerRecvMessage(getNativeHandle(this), flags | 0);
      }
    } catch (error) {
      throw recvNativeError(error, flags, 'recv failed');
    }
    if (raw == null) {
      setBufferedReceive(this, false);
      return false;
    }
    // A ROUTER recv can always route a reply back to the source by routing id;
    // the request sequence only gates the correlated reply() path, not send().
    const send = raw.routingId == null
      ? undefined
      : (parts: readonly Message[], sendFlags: SendFlags) => {
        if (!raw.routingId) {
          throw submitErrorFromResult(SubmitResult.InvalidState, 'missing routed send target');
        }
        return this.sendDirectRaw(raw.routingId, parts, sendFlags);
      };
    const reply = raw.routingId == null || raw.requestSeq == null || raw.requestSeq === 0n
      ? undefined
      : (requestSeq: bigint, parts: readonly Message[], replyFlags: SendFlags) => {
        if (!raw.routingId) {
          throw submitErrorFromResult(SubmitResult.InvalidState, 'missing request reply target');
        }
        this.replyToRoutedMessage(
          RoutingId.from(raw.routingId), requestSeq, parts, replyFlags
        );
      };
    materializeReceivedInto(result, raw, reply, send);
    return true;
  }

  close(): void {
    this.bufferedReceive.drain((raw) => {
      const pending = new Received();
      materializeReceivedInto(pending, raw);
      pending.close();
    });
    super.close();
  }
}
