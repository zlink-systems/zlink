// SPDX-License-Identifier: MPL-2.0

import type {
  NativeHandle,
  NativeReceivedRaw,
  NativeTopicMessageRaw,
  SubscriptionEntry
} from './binding_types';

interface NativeRoutedAttemptResult {
  result: number;
  nativeErrno: number;
}

interface NativeRoutedTargetResult extends NativeRoutedAttemptResult {
  peerRid?: Buffer;
  transportPairId?: bigint;
  transportPairGeneration?: bigint;
}

interface NativeRoutedReadyEvent {
  peerRid: Buffer;
  transportPairId: bigint;
  transportPairGeneration: bigint;
  state: number;
  terminalErrno: number;
}

export interface SocketNativeBinding {
  dealerRoutedSendAttempt: (
    socket: NativeHandle,
    peerRid: Buffer,
    transportPairId: bigint,
    transportPairGeneration: bigint,
    parts: readonly Buffer[]
  ) => NativeRoutedAttemptResult;
  dealerRoutedRequestAttempt: (
    socket: NativeHandle,
    peerRid: Buffer,
    transportPairId: bigint,
    transportPairGeneration: bigint,
    parts: readonly Buffer[],
    token: bigint,
    timeoutMs: number
  ) => NativeRoutedAttemptResult;
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
    cachedRoutingId?: Buffer | null
  ) => NativeReceivedRaw | null;
  routerRecvMessageNoWait: (
    socket: NativeHandle,
    preferManagedSinglePart?: boolean,
    cachedRoutingId?: Buffer | null
  ) => NativeReceivedRaw | null;
  routerRecvMessageRetained: (socket: NativeHandle, flags: number) => NativeReceivedRaw | null;
  routerRecvMessageRetainedNoWait: (socket: NativeHandle) => NativeReceivedRaw | null;
  routerReply: (
    socket: NativeHandle,
    peerRid: Buffer,
    requestSeq: bigint,
    parts: unknown
  ) => void;
  routerRoutedSendAttempt: (
    socket: NativeHandle,
    peerRid: Buffer,
    transportPairId: bigint,
    transportPairGeneration: bigint,
    parts: readonly Buffer[]
  ) => NativeRoutedAttemptResult;
  streamRoutedSendAttempt: (
    socket: NativeHandle,
    peerRid: Buffer,
    transportPairId: bigint,
    transportPairGeneration: bigint,
    parts: readonly Buffer[]
  ) => NativeRoutedAttemptResult;
  routerRoutedRequestAttempt: (
    socket: NativeHandle,
    peerRid: Buffer,
    transportPairId: bigint,
    transportPairGeneration: bigint,
    parts: readonly Buffer[],
    token: bigint,
    timeoutMs: number
  ) => NativeRoutedAttemptResult;
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
  socketRecvMessageRetained: (socket: NativeHandle, flags: number) => NativeReceivedRaw | null;
  socketRecvMessageRetainedNoWait: (socket: NativeHandle) => NativeReceivedRaw | null;
  dealerRecvMessageRetained: (socket: NativeHandle, flags: number) => NativeReceivedRaw | null;
  dealerRecvMessageRetainedNoWait: (socket: NativeHandle) => NativeReceivedRaw | null;
  hwmBudgetLeaseRelease: (owner: unknown) => void;
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
  socketRoutedSendReadyHandler: (
    socket: NativeHandle,
    handler: (event: NativeRoutedReadyEvent) => void
  ) => void;
  socketRoutedAdmissionClose: (socket: NativeHandle) => void;
  socketRoutedAdmissionBeginClose: (socket: NativeHandle) => void;
  socketSelectRoutedSubmitTarget: (
    socket: NativeHandle,
    routerRid: Buffer | null
  ) => NativeRoutedTargetResult;
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
  socketSubscribeMessageRetained: (
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
  socketTrySubscribeMessageRetained: (socket: NativeHandle) => NativeTopicMessageRaw | null;
  socketTrySubscriptionEvent: (
    socket: NativeHandle
  ) => { routingId?: Buffer | null; topic: string; subscribed: boolean } | null;
  socketUnbind: (socket: NativeHandle, endpoint: string) => void;
  socketUnsetSubscription: (socket: NativeHandle, topic: string) => void;
  subscriptionAt: (socket: NativeHandle, index: number) => SubscriptionEntry | null;
}
