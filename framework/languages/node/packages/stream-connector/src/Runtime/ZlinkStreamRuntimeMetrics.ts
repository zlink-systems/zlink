import type { RequiredZlinkStreamConnectorOptions } from '../Contracts';

export class ZlinkStreamRuntimeMetrics {
  private readonly reconnects;
  private readonly handshakeDuration;
  private readonly handshakeFailures;
  private readonly inboundBytes;
  private readonly outboundBytes;

  constructor(private readonly options: RequiredZlinkStreamConnectorOptions) {
    const meter = options.meterProvider?.getMeter('zlink.framework');
    this.reconnects = meter?.createCounter('zlink.stream.reconnects', { unit: '{event}' });
    this.handshakeDuration = meter?.createHistogram('zlink.stream.handshake.duration', { unit: 's' });
    this.handshakeFailures = meter?.createCounter('zlink.stream.handshake.failures', { unit: '{failure}' });
    this.inboundBytes = meter?.createCounter('zlink.stream.inbound.bytes', { unit: 'By' });
    this.outboundBytes = meter?.createCounter('zlink.stream.outbound.bytes', { unit: 'By' });
  }

  reconnect(): void {
    this.safe(() => this.reconnects?.add(1, { transport: this.transportLabel() }));
  }

  handshakeCompleted(startedAt: number): void {
    const seconds = (performance.now() - startedAt) / 1000;
    this.safe(() => this.handshakeDuration?.record(seconds, { transport: this.transportLabel() }));
  }

  handshakeFailed(error: unknown): void {
    const reason = error instanceof Error && (error.name === 'AbortError' || error.name === 'TimeoutError')
      ? 'canceled'
      : 'transport_error';
    this.safe(() => this.handshakeFailures?.add(1, { transport: this.transportLabel(), reason }));
  }

  inbound(byteCount: number): void {
    this.safe(() => this.inboundBytes?.add(byteCount, { transport: this.transportLabel() }));
  }

  outbound(byteCount: number): void {
    this.safe(() => this.outboundBytes?.add(byteCount, { transport: this.transportLabel() }));
  }

  private transportLabel(): 'ws' | 'wss' {
    return this.options.endpoint.startsWith('wss:') ? 'wss' : 'ws';
  }

  private safe(record: () => void): void {
    try {
      record();
    } catch {
      // Metrics listeners are external observation code and cannot change connector flow.
    }
  }
}
