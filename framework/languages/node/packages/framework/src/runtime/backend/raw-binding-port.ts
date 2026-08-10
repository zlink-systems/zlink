export interface ZLinkRawReceivedRecord {
  readonly sourceRid: string;
  readonly sourceRoute: Uint8Array;
  readonly requestSeq?: bigint;
  readonly transportPairId?: bigint;
  readonly transportPairGeneration?: bigint;
  readonly reply?: (parts: readonly Uint8Array[]) => void;
  readonly parts: readonly Buffer[];
}

export interface ZLinkRawMonitorRecord {
  readonly event: number;
  readonly value: number;
  readonly routingId?: string;
  readonly localAddress: string;
  readonly remoteAddress: string;
  readonly connectionId?: bigint;
  readonly transportPairId?: bigint;
  readonly transportPairGeneration?: bigint;
  readonly transportLane?: number;
  readonly flags?: number;
}

export interface ZLinkRawSocketPort {
  bind(endpoint: string): void;
  unbind(endpoint: string): void;
  connect(endpoint: string): void;
  disconnect(endpoint: string): void;
  monitor(handler: (event: ZLinkRawMonitorRecord) => void): ZLinkRawMonitorPort;
  close(): void;
}

export interface ZLinkRawMonitorPort {
  statusReady(): boolean;
  close(): void;
}

export interface ZLinkRawRouterPort extends ZLinkRawSocketPort {
  disconnectRid?(routingId: string): void;
  disconnectTransportPair?(transportPairId: bigint, transportPairGeneration: bigint): void;
  localEndpoint(): string;
  setRoutingId(routingId: string): void;
  connectToRoutingId(routingId: string, endpoint: string): void;
  send(targetRid: string, parts: readonly Uint8Array[], dontWait?: boolean): boolean;
  sendTransportPair(
    targetRid: string,
    transportPairId: bigint,
    transportPairGeneration: bigint,
    parts: readonly Uint8Array[],
    dontWait?: boolean
  ): boolean;
  request(
    targetRid: string,
    parts: readonly Uint8Array[],
    timeoutMs: number
  ): Promise<readonly Buffer[]>;
  requestTransportPair(
    targetRid: string,
    transportPairId: bigint,
    transportPairGeneration: bigint,
    parts: readonly Uint8Array[],
    timeoutMs: number
  ): Promise<readonly Buffer[]>;
  receive(dontWait?: boolean): ZLinkRawReceivedRecord | undefined;
  reply(
    targetRid: string | Uint8Array,
    requestSeq: bigint,
    parts: readonly Uint8Array[]
  ): void;
  trySendCompletionControl?(
    targetRid: string,
    parts: readonly Uint8Array[]
  ): boolean;
  trySendCompletionControlTransportPair?(
    targetRid: string,
    transportPairId: bigint,
    transportPairGeneration: bigint,
    parts: readonly Uint8Array[]
  ): boolean;
  setCompletionControlHandler?(
    handler: (sourceRid: string, parts: readonly Buffer[]) => void
  ): void;
  progressCompletion?(): number;
}

export interface ZLinkRawDealerPort extends ZLinkRawSocketPort {
  setRoutingId(routingId: string): void;
  send(parts: readonly Uint8Array[], dontWait?: boolean): boolean;
  request(parts: readonly Uint8Array[], timeoutMs: number): Promise<readonly Buffer[]>;
  receive(dontWait?: boolean): ZLinkRawReceivedRecord | undefined;
}

export interface ZLinkRawHostPort {
  createRouter(): ZLinkRawRouterPort;
  createDealer(): ZLinkRawDealerPort;
  shutdown(): void;
  close(): void;
}

export interface ZLinkRawBindingPort {
  createHost(): ZLinkRawHostPort;
}
