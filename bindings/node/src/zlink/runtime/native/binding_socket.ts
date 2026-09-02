// SPDX-License-Identifier: MPL-2.0

import type {
  NativeHandle,
  NativeReceivedRaw,
  NativeTopicMessageRaw,
  SubscriptionEntry
} from './binding_types';
import type {
  NativeCompletion,
  NativeSubmitResult,
  NativeSyncRequestResult,
} from '../messaging/completion_owner';

export interface SocketNativeBinding {
  socketSubmitSend: (
    socket: NativeHandle,
    parts: unknown,
    routingId: Buffer | null,
    flags: number,
    token: bigint
  ) => NativeSubmitResult;
  socketSubmitRequest: (
    socket: NativeHandle,
    target: Buffer | null,
    parts: unknown,
    timeoutMs: number,
    flags: number,
    token: bigint
  ) => NativeSubmitResult;
  socketRequestSync: (
    socket: NativeHandle,
    target: Buffer | null,
    parts: unknown,
    timeoutMs: number
  ) => NativeSyncRequestResult;
  socketCompletionRecv: (socket: NativeHandle, flags: number) => NativeCompletion | null;
  socketReply: (socket: NativeHandle, sourceRid: Buffer, replyToken: bigint, parts: unknown) => void;
  socketStreamRecvPacket: (
    socket: NativeHandle,
    flags: number
  ) => { routingId: Buffer; header: Buffer; body: Buffer } | null;
  handleGetRoutingId: (handle: NativeHandle) => Buffer;
  handleSetRoutingId: (handle: NativeHandle, routingId: Buffer) => void;
  monitorOpen: (
    socket: NativeHandle,
    eventMask: number,
    monitorHwmBytes: bigint
  ) => NativeHandle;
  routerRecvMessage: (
    socket: NativeHandle,
    flags: number,
    preferManagedSinglePart?: boolean,
    routingIdStorage?: Buffer | null
  ) => NativeReceivedRaw | null;
  routerRecvMessageNoWait: (
    socket: NativeHandle,
    preferManagedSinglePart?: boolean,
    routingIdStorage?: Buffer | null
  ) => NativeReceivedRaw | null;
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
  socketSetOpt: (socket: NativeHandle, option: number, value: Buffer) => void;
  socketSetReceiveFlowState: (socket: NativeHandle, state: number) => void;
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
