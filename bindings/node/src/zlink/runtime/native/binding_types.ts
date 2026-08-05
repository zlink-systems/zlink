// SPDX-License-Identifier: MPL-2.0

import type {
  NativeReceivedRaw,
  NativeTopicMessageRaw
} from '../messaging/message_materializer';
import type {
  MonitorEventValueRaw,
  MonitorStatusRaw
} from '../eventing/monitor_raw';
import type {
  SubscriptionEntry
} from '../../contracts/messaging';

export type NativeHandle = unknown;
export type NativeBuffer = Buffer;
export type NullableNativeHandle = NativeHandle | null;
export type NativeVersion = [number, number, number];
export type NativeRequestCallback = (
  result: number,
  replyParts: Buffer[] | null
) => void;
export type NativeCompletionControlHandler = (
  sourceRoutingId: Buffer,
  parts: Buffer[]
) => void;

export type {
  MonitorEventValueRaw,
  MonitorStatusRaw,
  NativeReceivedRaw,
  NativeTopicMessageRaw,
  SubscriptionEntry
};
