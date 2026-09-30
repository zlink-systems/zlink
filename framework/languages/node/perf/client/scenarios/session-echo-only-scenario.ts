import type { ZlinkStreamConnector } from '@zlink-systems/stream-connector';
import { PerfClock } from '../../shared/clock';
import { DecimalText, EndpointManifest, PerfEchoReply } from '../../shared/contracts';
import { Measurement } from '../../shared/measurement';
import { PayloadPattern } from '../../shared/payload';

// §11.1: independent CS process -> Session process; physical connector per global client ID.
// request().submit<PerfEchoReply>() through full identity/byte validation is one operation.
// 1024/4096 JSON, request/ordinary, Immediate public dispatch mode; no Store or Actor.
// Server logical stream, Actor, Spot, worker and fanout metrics are not applicable.
// The two CS Actor scenarios of §10.1 and §10.2 run this same connector loop: their setup probe names the connector ID,
// and the server side creates and binds that connector's Actor.
export class SessionEchoOnlyScenario {
  private readonly connectors: { id: number; local: number; connector: ZlinkStreamConnector }[] = [];
  private readonly owned: ZlinkStreamConnector[] = [];
  private sequences: number[] = [];
  private readonly count: number;
  private readonly first: number;

  constructor(private readonly manifest: EndpointManifest, private readonly measurement: Measurement, index: number) {
    const workload = manifest.workload;
    const total = workload.connections as number;
    const quotient = Math.floor(total / workload.clientCount);
    const remainder = total % workload.clientCount;
    this.count = quotient + (index < remainder ? 1 : 0);
    this.first = index * quotient + Math.min(index, remainder);
  }

  async prepare(): Promise<void> {
    const workload = this.manifest.workload;
    // The connector package is ESM-only; a dynamic import keeps this CommonJS process a plain public consumer of it.
    const { zlinkStreamConnectorFactory, zlinkStreamJsonCodec, ZlinkStreamDispatchMode } = await import('@zlink-systems/stream-connector');
    const endpoint = this.manifest.roles.find((role) => role.streamEndpoint !== null)!.streamEndpoint!;
    this.sequences = new Array<number>(this.count).fill(0);
    const evidence: unknown[] = new Array<unknown>(this.count);
    let next = 0;
    const worker = async (): Promise<void> => {
      for (;;) {
        const local = next++;
        if (local >= this.count) return;
        const id = this.first + local;
        const started = PerfClock.now();
        const connector = zlinkStreamConnectorFactory.create({
          endpoint, codec: zlinkStreamJsonCodec, dispatchMode: ZlinkStreamDispatchMode.Immediate,
          connectTimeoutMs: workload.setupTimeoutMs, requestTimeoutMs: workload.requestTimeoutMs
        });
        this.owned.push(connector);
        try {
          await connector.connect(AbortSignal.timeout(workload.setupTimeoutMs));
          const request = this.measurement.request(id, ++this.sequences[local], true);
          // Setup probe: bounded by the setup deadline; an Actor cell's probe also carries the session's create and bind.
          const reply = await connector.request(request).timeout(workload.setupTimeoutMs).submit<PerfEchoReply>(AbortSignal.timeout(workload.setupTimeoutMs));
          PayloadPattern.validateIdentity(request, reply);
          this.measurement.pattern.validate(reply.payload);
          if (!connector.isConnected) throw new Error('Connector lost its connection during setup.');
          this.connectors.push({ id, local, connector });
          this.measurement.connected++;
          evidence[local] = { kind: 'connectorSetupAndTypedProbe', source: 'connect + isConnected + request().submit<PerfEchoReply>',
            observedValue: { clientId: id, state: connector.state, isConnected: connector.isConnected, setupLatencyNs: (PerfClock.now() - started).toString(), correlationId: request.correlationId } };
        } catch (error) {
          this.measurement.connectionFailures++;
          evidence[local] = { kind: 'connectorSetupFailure', source: error instanceof Error ? error.constructor.name : typeof error,
            observedValue: { clientId: id, message: error instanceof Error ? error.message : String(error), setupLatencyNs: (PerfClock.now() - started).toString() } };
        }
      }
    };
    await Promise.all(Array.from({ length: Math.min(workload.connectConcurrency as number, this.count) }, worker));
    this.measurement.setupEvidence = evidence;
  }

  run = (): Promise<void> => Promise.all(this.connectors.flatMap((entry) =>
    Array.from({ length: this.manifest.workload.inflight }, () => this.loop(entry.id, entry.local, entry.connector)))).then(() => undefined);

  private async loop(id: number, local: number, connector: ZlinkStreamConnector): Promise<void> {
    const measurement = this.measurement;
    while (measurement.canIssue) {
      let request = measurement.request(id, ++this.sequences[local]);
      const started = measurement.beginOperation();
      if (started === undefined) break;
      request = request.with({ sentTicks: DecimalText.of(started) });
      try {
        const reply = await connector.request(request).timeout(this.manifest.workload.requestTimeoutMs).submit<PerfEchoReply>();
        PayloadPattern.validateIdentity(request, reply);
        measurement.pattern.validate(reply.payload);
        measurement.completeOperation(started);
      } catch (error) {
        measurement.completeOperation(started, error);
      }
    }
  }

  async dispose(): Promise<void> {
    const closed = await Promise.allSettled(this.owned.map((connector) => connector.close()));
    const failures = closed.flatMap((result) => result.status === 'rejected' ? [result.reason] : []);
    if (failures.length > 0) {
      const details = failures.map((failure) => failure instanceof Error ? `${failure.constructor.name}: ${failure.message}` : String(failure));
      throw new AggregateError(failures, `Failed to close STREAM connector(s): ${details.join('; ')}`);
    }
  }
}
