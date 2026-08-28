// SPDX-License-Identifier: MPL-2.0

import type {
  NativeHandle,
  NativeReceivedRaw,
  NativeTopicMessageRaw,
  SubscriptionEntry
} from './binding_types';

export interface NativeSendCompletionEvent {
  token: bigint;
  opId: bigint;
  result: number;
  terminalErrno: number;
  peerRid: Buffer;
  transportPairId: bigint;
  transportPairGeneration: bigint;
}

export interface NativeSendSubmitResult {
  result: number;
  nativeErrno: number;
  opId: bigint;
  inlineCompletion?: NativeSendCompletionEvent;
}

export interface SocketNativeBinding {
  socketSendCompletionHandler: (
    socket: NativeHandle,
    handler: (event: NativeSendCompletionEvent) => void
  ) => void;
  socketSendAsync: (
    socket: NativeHandle,
    parts: unknown,
    timeoutMs: number,
    routingId: Buffer | null,
    token: bigint
  ) => NativeSendSubmitResult;
  dealerRequest: (
    socket: NativeHandle,
    parts: unknown,
    token: bigint,
    timeoutMs: number
  ) => { result: number; nativeErrno: number };
  dealerReply: (
    socket: NativeHandle,
    requestSeq: bigint,
    parts: unknown
  ) => void;
  dealerRecvMessage: (
    socket: NativeHandle,
    flags: number
  ) => NativeReceivedRaw | null;
  dealerRecvMessageNoWait: (socket: NativeHandle) => NativeReceivedRaw | null;
  routerRequest: (
    socket: NativeHandle,
    peerRid: Buffer,
    parts: unknown,
    token: bigint,
    timeoutMs: number,
    transportPairId?: bigint,
    transportPairGeneration?: bigint
  ) => { result: number; nativeErrno: number };
  socketRequestCompletionHandler: (socket: NativeHandle, handler: unknown) => void;
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
  routerReply: (
    socket: NativeHandle,
    peerRid: Buffer,
    requestSeq: bigint,
    parts: unknown
  ) => void;
  routerSendTransportPair: (
    socket: NativeHandle,
    peerRid: Buffer,
    transportPairId: bigint,
    transportPairGeneration: bigint,
    parts: unknown,
    flags: number
  ) => void;
  socketBind: (socket: NativeHandle, endpoint: string) => void;
  socketClose: (socket: NativeHandle) => void;
  socketConnect: (socket: NativeHandle, endpoint: string) => void;
  socketDisconnect: (socket: NativeHandle, endpoint: string) => void;
  socketDisconnectRid: (socket: NativeHandle, routingId: Buffer) => void;
  socketDisconnectTransportPair: (
    socket: NativeHandle,
    transportPairId: bigint,
    transportPairGeneration: bigint
  ) => void;
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
  socketStreamSendRoutingNoWaitResultParts: (
    socket: NativeHandle,
    routingId: Buffer,
    parts: readonly unknown[]
  ) => number;
  socketStreamSendRoutingParts: (
    socket: NativeHandle,
    routingId: Buffer,
    parts: readonly unknown[],
    flags: number
  ) => void;
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
  socketStreamAttach: (
    socket: NativeHandle,
    handler: (routingId: Buffer | null, header: Buffer, body: unknown) => number,
    packetCount: number,
    bodyMaterialization: number
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
