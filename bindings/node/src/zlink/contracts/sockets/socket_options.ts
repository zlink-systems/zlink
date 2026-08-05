// SPDX-License-Identifier: MPL-2.0

import type { Message, MessageLike } from '../messaging';
import type { RoutingId } from '../core';
import type { RidDuplicatePolicyValue } from './socket_constants';

/** Typed facade over the socket options shared by every socket type. */
export interface CommonSocketOptions {
  /** How long, in ms, close waits to deliver queued messages. */
  linger: number;
  /** Maximum accounted outbound bytes before back-pressure; 0n means unlimited. */
  sendHwm: bigint;
  /** Maximum accounted inbound bytes before back-pressure; 0n means unlimited. */
  recvHwm: bigint;
  /** How long, in ms, a blocking send waits before failing. */
  sendTimeout: number;
  /** How long, in ms, a blocking receive waits before failing. */
  recvTimeout: number;
  /** Whether messages queue only to fully established connections. */
  immediate: boolean;
  /** How the socket reacts when a peer reuses an existing routing id. */
  ridDuplicatePolicy: RidDuplicatePolicyValue;
  /** Time limit, in ms, for a single connection attempt. */
  connectTimeout: number;
  /** Whether IPv6 connections are enabled. */
  ipv6: boolean;
  /** Whether Nagle's algorithm is disabled (`TCP_NODELAY`). */
  tcpNoDelay: boolean;
  /** TCP keep-alive: -1 OS default, 0 off, 1 on. */
  tcpKeepalive: number;
  /** Maximum inbound message size in bytes; -1n means no limit. */
  maxMsgSize: bigint;
  /** The concrete endpoint the socket last bound to (resolves wildcards). */
  readonly lastEndpoint: string;
  /** Maximum length of the pending-connection queue on a bound endpoint. */
  backlog: number;
  /** Base delay, in ms, before reconnecting a broken connection. */
  reconnectInterval: number;
  /** Maximum reconnect delay, in ms, under exponential backoff. */
  reconnectIntervalMax: number;
  /** Whether a submit that fails locally is retried (see RidDuplicatePolicy/SubmitRetryMode constants). */
  submitRetryMode: number;
  /** Total time budget, in ms, for submit retries. */
  submitRetryTimeout: number;
  /** Maximum number of submit retry attempts. */
  submitRetryAttempts: number;
}

/** Typed facade over DEALER-specific socket options. */
export interface DealerSocketOptions extends CommonSocketOptions {
  /** Whether to send an empty probe on connect so the peer learns this socket's routing id. */
  probe: boolean;
  /** How long, in ms, a DEALER request waits for a reply. */
  requestTimeout: number;
  /** This peer's relative load-balancing weight (0-100). */
  peerWeight: number;
}

/** Typed facade over ROUTER-specific socket options. */
export interface RouterSocketOptions extends CommonSocketOptions {
  /** Whether sending to an unknown route raises an error instead of dropping the message. */
  mandatory: boolean;
  /** Whether a new peer reusing an existing routing id takes it over. */
  handover: boolean;
  /** Whether to send an empty probe on connect so the peer learns this socket's routing id. */
  probe: boolean;
  /** The routing id assigned to the next outgoing connection, or null when none is pending. */
  readonly connectRoutingId: RoutingId | null;
  /** Assign `routingId` to the next connection this socket initiates. */
  setConnectRoutingId(routingId: RoutingId): void;
  /** How long, in ms, a ROUTER request waits for a reply. */
  requestTimeout: number;
  /** This peer's relative load-balancing weight (0-100). */
  peerWeight: number;
}

/** Typed facade over STREAM-specific socket options. */
export interface StreamSocketOptions extends CommonSocketOptions {
  /** Whether peer connect and disconnect events are delivered as messages. */
  notify: boolean;
}

/** Typed facade over PUB/XPUB-specific socket options. */
export interface PubSocketOptions extends CommonSocketOptions {
  /** Whether every subscription message (including duplicates) is delivered. */
  verbose: boolean;
  /** Whether every subscription and unsubscription message is delivered. */
  verboser: boolean;
  /** Whether a back-pressured send raises an error instead of dropping the message. */
  noDrop: boolean;
  /** Whether subscriptions are handled manually via approve/reject. */
  manual: boolean;
  /** Manual handling that replays the last cached message per topic to a new subscriber. */
  manualLastValue: boolean;
  /** The number of distinct topics currently subscribed across subscribers. */
  readonly topicsCount: number;
  /** Return the welcome message sent to new subscribers (caller owns it). */
  welcomeMessage(): Message;
  /** Set a message automatically sent to each newly connected subscriber. */
  setWelcomeMessage(message: MessageLike): void;
  /** Accept a pending subscription from `routingId`; requires manual mode. */
  approveSubscribe(routingId: RoutingId): void;
  /** Decline a pending subscription from `routingId`; requires manual mode. */
  rejectSubscribe(routingId: RoutingId): void;
}

/** Typed facade over SUB-specific socket options. */
export interface SubSocketOptions extends CommonSocketOptions {
  /** The number of active subscriptions on this socket. */
  readonly topicsCount: number;
}
