export interface ZLinkRawReceivedRecord {
  readonly sourceRid: string;
  readonly sourceRoute: Uint8Array;
  readonly requestSeq?: bigint;
  readonly transportPairId?: bigint;
  readonly transportPairGeneration?: bigint;
  readonly reply?: (parts: readonly Uint8Array[]) => void;
  readonly parts: readonly Buffer[];
  /** Releases the ordinary Framework-owned receive record exactly once. */
  close(): void;
}

export interface ZLinkRawMonitorRecord {
  readonly event: number;
  /** Lossless native monitor value (Core exposes the full unsigned 64-bit field). */
  readonly value: bigint;
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
  setReceiveFlowState(state: 'running' | 'paused'): void;
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
  send(targetRid: string, parts: readonly Uint8Array[]): Promise<void>;
  request(
    targetRid: string,
    parts: readonly Uint8Array[],
    timeoutMs: number
  ): Promise<readonly Buffer[]>;
  receive(dontWait?: boolean): ZLinkRawReceivedRecord | undefined;
  reply(
    targetRid: string | Uint8Array,
    requestSeq: bigint,
    parts: readonly Uint8Array[]
  ): void;
}

export interface ZLinkRawDealerPort extends ZLinkRawSocketPort {
  setRoutingId(routingId: string): void;
  send(parts: readonly Uint8Array[]): Promise<void>;
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
