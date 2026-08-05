export interface ServiceProbe {
  readonly nodeRoutingId: string;
  readonly connectionId: string;
  readonly probeId: bigint;
}

export interface ServiceLivenessTick {
  readonly probes: readonly ServiceProbe[];
  readonly timedOutNodes: readonly string[];
}

interface PeerState {
  readonly connectionId: string;
  deadlineMs: number;
  nextProbeMs: number;
  outstandingProbe?: bigint;
  ready: boolean;
}

export const DEFAULT_SERVICE_PROBE_INTERVAL_MS = 5_000;
export const DEFAULT_SERVICE_PEER_TIMEOUT_MS = 15_000;

/** Uses monotonic milliseconds and never treats application traffic as a probe ACK. */
export class ServiceLivenessRegistry {
  private readonly peers = new Map<string, PeerState>();
  private nextProbeId = 1n;

  constructor(
    private readonly probeIntervalMs = DEFAULT_SERVICE_PROBE_INTERVAL_MS,
    private readonly peerTimeoutMs = DEFAULT_SERVICE_PEER_TIMEOUT_MS
  ) {
    if (
      !Number.isFinite(probeIntervalMs)
      || probeIntervalMs <= 0
      || !Number.isFinite(peerTimeoutMs)
      || peerTimeoutMs <= probeIntervalMs
    ) {
      throw new RangeError('Liveness requires a positive probe interval and a larger timeout.');
    }
  }

  admit(nodeRoutingId: string, connectionId: string, nowMs: number): void {
    requireIdentity(nodeRoutingId, 'nodeRoutingId');
    requireIdentity(connectionId, 'connectionId');
    const current = this.peers.get(nodeRoutingId);
    if (current?.connectionId === connectionId) return;
    this.peers.set(nodeRoutingId, {
      connectionId,
      deadlineMs: nowMs + this.peerTimeoutMs,
      nextProbeMs: nowMs + this.probeIntervalMs,
      ready: false
    });
  }

  requestProbe(nodeRoutingId: string, connectionId: string, nowMs: number): boolean {
    const current = this.peers.get(nodeRoutingId);
    if (
      current === undefined
      || current.connectionId !== connectionId
      || current.ready
      || current.outstandingProbe !== undefined
    ) return false;
    current.nextProbeMs = Math.min(current.nextProbeMs, nowMs);
    return true;
  }

  disconnect(nodeRoutingId: string, connectionId: string): boolean {
    const current = this.peers.get(nodeRoutingId);
    if (current === undefined || current.connectionId !== connectionId) return false;
    this.peers.delete(nodeRoutingId);
    return true;
  }

  acknowledge(
    nodeRoutingId: string,
    connectionId: string,
    probeId: bigint,
    nowMs: number
  ): boolean {
    const current = this.peers.get(nodeRoutingId);
    if (
      current === undefined
      || current.connectionId !== connectionId
      || current.outstandingProbe !== probeId
    ) {
      return false;
    }
    current.outstandingProbe = undefined;
    current.deadlineMs = nowMs + this.peerTimeoutMs;
    current.ready = true;
    return true;
  }

  acknowledgeProbe(
    nodeRoutingId: string,
    connectionId: string,
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
      if (peer.deadlineMs <= nowMs) {
        timedOutNodes.push(nodeRoutingId);
        this.peers.delete(nodeRoutingId);
        continue;
      }
      if (peer.nextProbeMs > nowMs) continue;
      peer.outstandingProbe ??= this.allocateProbeId();
      probes.push({ nodeRoutingId, connectionId: peer.connectionId, probeId: peer.outstandingProbe });
      do {
        peer.nextProbeMs += this.probeIntervalMs;
      } while (peer.nextProbeMs <= nowMs);
    }
    timedOutNodes.sort();
    return { probes, timedOutNodes };
  }

  get size(): number {
    return this.peers.size;
  }

  isReady(nodeRoutingId: string, connectionId: string): boolean {
    const current = this.peers.get(nodeRoutingId);
    return current?.connectionId === connectionId && current.ready;
  }

  private allocateProbeId(): bigint {
    const result = this.nextProbeId++;
    if (this.nextProbeId > 0xffff_ffff_ffff_ffffn) this.nextProbeId = 1n;
    return result;
  }
}

function requireIdentity(value: string, field: string): void {
  if (value.length === 0) throw new TypeError(`${field} must be non-empty.`);
}
