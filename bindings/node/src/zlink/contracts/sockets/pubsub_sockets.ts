// SPDX-License-Identifier: MPL-2.0

import type { SubscriptionEvent, TopicMessage } from '../messaging';
import type { SendOperation } from '../messaging';
import type { SubscriptionEntry } from '../messaging';
import type { RecvFlags } from './socket_constants';
import type { PubSocketOptions, SubSocketOptions } from './socket_options';
import type { ConnectableSocket } from './socket';

/** PUB socket: topic-filtered publish that drops messages with no matching subscriber. */
export interface PubSocket extends ConnectableSocket {
  /** The PUB-specific typed options facade. */
  readonly options: PubSocketOptions;
  /** Begin publishing under `topic`; parts are consumed on a successful submit. */
  publish(topic: string): SendOperation;
  /**
   * Register a callback invoked when the socket can accept more sends after
   * back-pressure. The callback runs on a background dispatch thread.
   */
  setSendReadyHandler(handler: () => void): void;
}

/** XPUB socket: like PUB, but also surfaces subscriber subscription and unsubscription events. */
export interface XPubSocket extends PubSocket {
  /**
   * Receive the next subscriber (un)subscription event into `result`; false
   * when `RecvFlags.DontWait` is set and none is available.
   */
  receiveSubscriptionEvent(result: SubscriptionEvent, flags?: RecvFlags): boolean;
}

/** SUB socket: a receive-only subscriber filtered by its subscriptions. */
export interface SubSocket extends ConnectableSocket {
  /** The SUB-specific typed options facade. */
  readonly options: SubSocketOptions;
  /** Add a subscription for `filter` (an exact topic or pattern); subscriptions accumulate. */
  setSubscription(filter: string): void;
  /** Remove a subscription previously added for `filter`. */
  unsetSubscription(filter: string): void;
  /** Return the active subscription at `index`, or null when out of range. */
  subscriptionAt(index: number): SubscriptionEntry | null;
  /**
   * Receive the next matching topic message into `result`; false when
   * `RecvFlags.DontWait` is set and none is available.
   */
  subscribe(result: TopicMessage, flags?: RecvFlags): boolean;
}

/** XSUB socket: a subscriber whose subscriptions are carried as messages. */
export interface XSubSocket extends SubSocket {}
