import { UINT64_MAX } from '@zlink-systems/stream-wire';
export interface ServiceProbe {
  readonly nodeRoutingId: string;
  readonly connectionId: string | bigint;
  readonly probeId: bigint;
}

export interface ServiceLivenessTick {
  readonly probes: readonly ServiceProbe[];
  readonly timedOutNodes: readonly string[];
}

export const DEFAULT_SERVICE_PROBE_INTERVAL_MS = 5_000;
export const DEFAULT_SERVICE_PEER_TIMEOUT_MS = 15_000;

export class ServiceLivenessConnection {
  deadlineMs: number;
  nextProbeMs: number;
  outstandingProbe?: bigint;

  constructor(
    readonly connectionId: string | bigint,
    nowMs: number,
    private readonly probeIntervalMs = DEFAULT_SERVICE_PROBE_INTERVAL_MS,
    private readonly peerTimeoutMs = DEFAULT_SERVICE_PEER_TIMEOUT_MS
  ) {
    this.deadlineMs = nowMs + peerTimeoutMs;
    this.nextProbeMs = nowMs;
  }

  readonly recordReceived = (nowMs = performance.now()): void => {
    this.deadlineMs = Math.max(this.deadlineMs, nowMs + this.peerTimeoutMs);
  };

  isExpired(nowMs: number): boolean {
    return nowMs >= this.deadlineMs;
  }

  tryGetProbe(nowMs: number, allocateProbeId: () => bigint): bigint | undefined {
    if (nowMs < this.nextProbeMs) return undefined;
    this.outstandingProbe ??= allocateProbeId();
    do {
      this.nextProbeMs += this.probeIntervalMs;
    } while (this.nextProbeMs <= nowMs);
    return this.outstandingProbe;
  }

  acknowledge(probeId: bigint, nowMs: number): boolean {
    this.recordReceived(nowMs);
    if (this.outstandingProbe !== probeId) return false;
    this.outstandingProbe = undefined;
    return true;
  }
}

/** Uses monotonic milliseconds and never treats application traffic as a probe ACK. */
export class ServiceLivenessRegistry {
  private readonly peers = new Map<string, ServiceLivenessConnection>();
  private nextProbeId = 1n;

  constructor(
    private readonly probeIntervalMs = DEFAULT_SERVICE_PROBE_INTERVAL_MS,
    private readonly peerTimeoutMs = DEFAULT_SERVICE_PEER_TIMEOUT_MS
  ) {
    if (
      !Number.isFinite(probeIntervalMs) ||
      probeIntervalMs <= 0 ||
      !Number.isFinite(peerTimeoutMs) ||
      peerTimeoutMs <= probeIntervalMs
    ) {
      throw new RangeError('Liveness requires a positive probe interval and a larger timeout.');
    }
  }

  admit(
    nodeRoutingId: string,
    connectionId: string | bigint,
    nowMs: number
  ): ServiceLivenessConnection {
    requireIdentity(nodeRoutingId, 'nodeRoutingId');
    if (typeof connectionId === 'string') requireIdentity(connectionId, 'connectionId');
    else if (connectionId === 0n) throw new TypeError('connectionId must be nonzero.');
    const current = this.peers.get(nodeRoutingId);
    if (current?.connectionId === connectionId) return current;
    const admitted = new ServiceLivenessConnection(
      connectionId,
      nowMs,
      this.probeIntervalMs,
      this.peerTimeoutMs
    );
    this.peers.set(nodeRoutingId, admitted);
    return admitted;
  }

  recordReceived(connection: ServiceLivenessConnection, nowMs: number): void {
    connection.recordReceived(nowMs);
  }

  disconnect(nodeRoutingId: string, connectionId: string | bigint): boolean {
    const current = this.peers.get(nodeRoutingId);
    if (current === undefined || current.connectionId !== connectionId) return false;
    this.peers.delete(nodeRoutingId);
    return true;
  }

  acknowledge(
    nodeRoutingId: string,
    connectionId: string | bigint,
    probeId: bigint,
    nowMs: number
  ): boolean {
    const current = this.peers.get(nodeRoutingId);
    if (current === undefined || current.connectionId !== connectionId) return false;
    return current.acknowledge(probeId, nowMs);
  }

  acknowledgeProbe(
    nodeRoutingId: string,
    connectionId: string | bigint,
    probeId: bigint
  ): ServiceProbe | undefined {
    const current = this.peers.get(nodeRoutingId);
    if (probeId <= 0n || current === undefined || current.connectionId !== connectionId) {
      return undefined;
    }
    return { nodeRoutingId, connectionId, probeId };
  }

  tick(nowMs: number): ServiceLivenessTick {
    const probes: ServiceProbe[] = [];
    const timedOutNodes: string[] = [];
    for (const [nodeRoutingId, peer] of this.peers) {
      if (peer.isExpired(nowMs)) {
        timedOutNodes.push(nodeRoutingId);
        this.peers.delete(nodeRoutingId);
        continue;
      }
      const probeId = peer.tryGetProbe(nowMs, this.allocateProbeId);
      if (probeId !== undefined)
        probes.push({ nodeRoutingId, connectionId: peer.connectionId, probeId });
    }
    timedOutNodes.sort();
    return { probes, timedOutNodes };
  }

  get size(): number {
    return this.peers.size;
  }

  private readonly allocateProbeId = (): bigint => {
    const result = this.nextProbeId++;
    if (this.nextProbeId > UINT64_MAX) this.nextProbeId = 1n;
    return result;
  };
}

function requireIdentity(value: string, field: string): void {
  if (value.length === 0) throw new TypeError(`${field} must be non-empty.`);
}
