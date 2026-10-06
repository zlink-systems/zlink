import { Inject, Injectable } from '@nestjs/common';
import type {
  ZLinkClientServerRuntime,
  ZLinkMessageContext,
  ZLinkRouteClient,
  ZLinkRouteMeshRuntime
} from '@zlink-systems/framework';
import { PerfClock } from '../shared/clock';
import { DecimalText, PerfEchoReply, PerfEchoRequest, RoleConfig } from '../shared/contracts';
import { Measurement } from '../shared/measurement';
import { PayloadPattern } from '../shared/payload';
import { ObjectsReadiness } from '../server-support/server-application';
import { runRequestStreams, until } from '../server-support/wait';

// §11.2: two Channel processes, manual RouteMesh or ClientServer, no Store/objects.
// Source public request -> typed identity/full-byte validation is one operation.
// JSON payloads: 1024/4096, request/ordinary. Connector/Actor/Spot/worker/fanout metrics do not apply.
export class ChannelEchoOnlyScenario {
  private sequences: number[] = [];

  constructor(
    private readonly client: ZLinkRouteClient,
    private readonly measurement: Measurement,
    private readonly config: RoleConfig,
    private readonly meshRuntime: ZLinkRouteMeshRuntime,
    private readonly channelRuntime: ZLinkClientServerRuntime,
    private readonly readiness: ObjectsReadiness
  ) {}

  async prepare(): Promise<void> {
    const { config, measurement } = this;
    const timeoutMs = config.workload.setupTimeoutMs;
    try {
      let channelTargetStatus: unknown;
      // observe() is a change stream, not an initial snapshot (monitoring §6): query public status until setup evidence is ready.
      await until(
        () => {
          if (config.topology === 'routemesh') {
            const status = this.meshRuntime.snapshot(config.meshName!);
            const channel = status.channels.find((item) => item.channelName === config.channelName);
            if (status.isReady && channel?.isReady && channel.readyTargetCount > 0) {
              channelTargetStatus = channel;
              return true;
            }
            return false;
          }
          const status = this.channelRuntime.snapshot(config.channelName!);
          if (status.isReady && status.readyTargetCount > 0) {
            channelTargetStatus = status;
            return true;
          }
          return false;
        },
        timeoutMs,
        'the Channel target to be ready'
      );
      this.sequences = new Array<number>(config.workload.logicalStreams as number).fill(0);
      const request = measurement.request(0, ++this.sequences[0], true);
      const reply = await this.client
        .requestToChannel(config.channelName!, request)
        .timeout(measurement.callTimeout())
        .submit<PerfEchoReply>(AbortSignal.timeout(timeoutMs));
      PayloadPattern.validateIdentity(request, reply);
      measurement.pattern.validate(reply.payload);
      measurement.setupEvidence = [
        {
          kind: 'typedProbeEcho',
          source: 'ZLinkRouteClient.requestToChannel.submit<PerfEchoReply>',
          observedValue: {
            correlationId: request.correlationId,
            receivedTicks: reply.receivedTicks,
            clockDomainId: reply.clockDomainId
          }
        }
      ];
      this.readiness.set(true, '', [
        {
          kind: 'channelTarget',
          source:
            config.topology === 'routemesh'
              ? 'ZLinkRouteMeshRuntime.snapshot'
              : 'ZLinkClientServerRuntime.snapshot',
          observedValue: channelTargetStatus
        }
      ]);
    } catch (error) {
      measurement.recordDiagnostic(error);
    }
  }

  run = (): Promise<void> =>
    runRequestStreams(
      this.config.workload.logicalStreams as number,
      () => this.measurement.canIssue,
      (stream) => this.loop(stream),
      (error) => this.measurement.recordDiagnostic(error)
    );

  private async loop(stream: number): Promise<void> {
    const { config, measurement } = this;
    if (!measurement.canIssue) return;
    let request = measurement.request(stream, ++this.sequences[stream]);
    const started = measurement.beginOperation();
    if (started === undefined) return;
    request = request.with({ sentTicks: DecimalText.of(started) });
    try {
      const reply = await this.client
        .requestToChannel(config.channelName!, request)
        .timeout(measurement.callTimeout())
        .submit<PerfEchoReply>();
      PayloadPattern.validateIdentity(request, reply);
      measurement.pattern.validate(reply.payload);
      measurement.completeOperation(started);
    } catch (error) {
      measurement.completeOperation(started, error);
    }
  }
}

@Injectable()
export class ChannelEchoHandler {
  constructor(@Inject(Measurement) private readonly measurement: Measurement) {}

  async handle(request: PerfEchoRequest, _context: ZLinkMessageContext): Promise<PerfEchoReply> {
    const received = PerfClock.now();
    const measurement = this.measurement;
    measurement.handlerEnter();
    try {
      measurement.validateRequest(request);
      const reply = PayloadPattern.reply(request, received);
      measurement.recordReply(request);
      if (measurement.phase === 'setup')
        measurement.setupEvidence = [
          {
            kind: 'typedProbeReply',
            source: 'ZLinkRequestHandler<PerfEchoRequest,PerfEchoReply>',
            observedValue: request.correlationId
          }
        ];
      return reply;
    } catch (error) {
      measurement.recordDiagnostic(error);
      throw error;
    } finally {
      measurement.handlerExit();
    }
  }
}
