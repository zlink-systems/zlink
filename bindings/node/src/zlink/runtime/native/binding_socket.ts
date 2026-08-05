// SPDX-License-Identifier: MPL-2.0

import type {
  NativeHandle,
  NativeCompletionControlHandler,
  NativeReceivedRaw,
  NativeRequestCallback,
  NativeTopicMessageRaw,
  SubscriptionEntry
} from './binding_types';

export interface SocketNativeBinding {
  dealerRequest: (
    socket: NativeHandle,
    parts: unknown,
    callback: unknown,
    flags: number,
    timeoutMs: number
  ) => void;
  handleGetRoutingId: (handle: NativeHandle) => Buffer;
  handleSetRoutingId: (handle: NativeHandle, routingId: Buffer) => void;
  monitorOpen: (socket: NativeHandle, eventMask: number) => NativeHandle;
  routerRecvMessage: (socket: NativeHandle, flags: number) => NativeReceivedRaw | null;
  routerRecvMessageNoWait: (socket: NativeHandle) => NativeReceivedRaw | null;
  routerReply: (
    socket: NativeHandle,
    peerRid: Buffer,
    requestSeq: bigint,
    parts: unknown
  ) => void;
  routerCompletionControlHandler: (
    socket: NativeHandle,
    handler: NativeCompletionControlHandler
  ) => void;
  routerTrySendCompletionControl: (
    socket: NativeHandle,
    peerRid: Buffer,
    parts: unknown
  ) => boolean;
  routerRequest: (
    socket: NativeHandle,
    peerRid: Buffer,
    parts: unknown,
    callback: NativeRequestCallback,
    flags: number,
    timeoutMs: number
  ) => void;
  socketBind: (socket: NativeHandle, endpoint: string) => void;
  socketClose: (socket: NativeHandle) => void;
  socketConnect: (socket: NativeHandle, endpoint: string) => void;
  socketDisconnect: (socket: NativeHandle, endpoint: string) => void;
  socketDisconnectRid: (socket: NativeHandle, routingId: Buffer) => void;
  socketGetOpt: (socket: NativeHandle, option: number) => Buffer;
  socketNew: (ctx: NativeHandle, type: number) => NativeHandle;
  socketPublish: (
    socket: NativeHandle,
    topic: string,
    payload: unknown,
    flags: number
  ) => number;
  socketRecvMessage: (socket: NativeHandle, flags: number) => NativeReceivedRaw | null;
  socketRecvMessageNoWait: (socket: NativeHandle) => NativeReceivedRaw | null;
  socketSend: (socket: NativeHandle, payload: unknown, flags: number) => void;
  socketSendNoWaitResult: (socket: NativeHandle, payload: unknown) => number;
  socketSendNoWaitResultParts: (
    socket: NativeHandle,
    parts: readonly unknown[]
  ) => number;
  socketSendParts: (
    socket: NativeHandle,
    parts: readonly unknown[],
    flags: number
  ) => void;
  socketSendReadyHandler: (socket: NativeHandle, handler: unknown) => void;
  socketSendRouting: (
    socket: NativeHandle,
    routingId: Buffer,
    payload: unknown,
    flags: number
  ) => void;
  socketSendRoutingParts: (
    socket: NativeHandle,
    routingId: Buffer,
    parts: readonly unknown[],
    flags: number
  ) => void;
  socketSendRoutingNoWaitResult: (
    socket: NativeHandle,
    routingId: Buffer,
    payload: unknown
  ) => number;
  socketSendRoutingNoWaitResultParts: (
    socket: NativeHandle,
    routingId: Buffer,
    parts: readonly unknown[]
  ) => number;
  socketSetOpt: (socket: NativeHandle, option: number, value: Buffer) => void;
  socketSetSubscription: (socket: NativeHandle, topic: string) => void;
  socketSetTlsClient: (
    socket: NativeHandle,
    ca: string,
    hostname: string,
    trustSystem: number
  ) => void;
  socketSetTlsServer: (
    socket: NativeHandle,
    cert: string,
    key: string,
    requireClientCert: number
  ) => void;
  socketStreamAttach: (
    socket: NativeHandle,
    handler: (routingId: Buffer | null, packets: Buffer[]) => number,
    packetCount: number
  ) => void;
  socketSubscribeMessage: (
    socket: NativeHandle,
    flags: number
  ) => NativeTopicMessageRaw | null;
  socketSubscriptionEvent: (
    socket: NativeHandle,
    flags: number
  ) => { routingId?: Buffer | null; topic: string; subscribed: boolean } | null;
  socketTryPublish: (
    socket: NativeHandle,
    topic: string,
    payload: unknown
  ) => number;
  socketTrySubscribeMessage: (socket: NativeHandle) => NativeTopicMessageRaw | null;
  socketTrySubscriptionEvent: (
    socket: NativeHandle
  ) => { routingId?: Buffer | null; topic: string; subscribed: boolean } | null;
  socketUnbind: (socket: NativeHandle, endpoint: string) => void;
  socketUnsetSubscription: (socket: NativeHandle, topic: string) => void;
  subscriptionAt: (socket: NativeHandle, index: number) => SubscriptionEntry | null;
}
