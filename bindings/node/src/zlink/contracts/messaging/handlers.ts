// SPDX-License-Identifier: MPL-2.0

import type { RoutingId } from '../core';
import type { MonitorEvent } from '../eventing';
import type { Message } from './message';

/** Invoked when a socket can accept more sends after back-pressure. */
export type SocketSendReadyHandler = () => void;

/** Invoked for each inbound framed packet; the callback owns both messages. */
export type StreamPacketHandler = (
  sourceRid: RoutingId,
  header: Message,
  body: Message
) => void;

/** Invoked for each socket monitor event. */
export type SocketMonitorHandler = (event: MonitorEvent) => void;

/** One active subscription: a topic filter and whether it is a pattern. */
export interface SubscriptionEntry {
  readonly filter: string;
  readonly isPattern: boolean;
}
