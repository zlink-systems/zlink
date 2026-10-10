import { Inject, Injectable, Scope } from '@nestjs/common';
import {
  ZLINK_ROUTE_MESH_RUNTIME,
  ZLINK_SPOT_MANAGER,
  ZLINK_SPOT_OUTBOUND,
  zlinkSpotPacketHandler
} from '@zlink-systems/nestjs';
import type {
  ZLinkMessageContext,
  ZLinkRouteMeshRuntime,
  ZLinkSpotManager,
  ZLinkSpotOutbound,
  ZLinkSpotPacketHandler,
  ZLinkSpotRequestHandler
} from '@zlink-systems/framework';
import { PerfClock } from '../shared/clock';
import {
  PerfDriveReply,
  PerfDriveRequest,
  PerfEchoReply,
  PerfEchoRequest,
  RoleConfig
} from '../shared/contracts';
import { Measurement } from '../shared/measurement';
import { ScenarioMetrics } from '../server-support/scenario-metrics';
import { SendSendCorrelation } from '../server-support/send-send-correlation';
import { ObjectsReadiness, ROLE_CONFIG, runRole } from '../server-support/server-application';
import { runTerminalStreams, until } from '../server-support/wait';
import { ActorlessSpot, configureSpotRole, createSpots, publishSpots } from './spot-role';

// §10.6 s2s-spot-to-channel-send-send-echo. Question: what a send from a Spot to a Channel that comes back to the
// original Spot as a separate send costs in completion rate and time. Roles: HTTP Client x1, Spot process (Object
// Server + local public driver, this file) x1, Channel target (Object Client) x1. The driver sends PerfDriveRequest
// to the Spot with ZLinkSpotOutbound.requestToSpot; the Spot handler registers the correlation, makes the first
// sendToChannel and returns once that send is admitted, so the turn is free when the Channel's send comes back to
// the Spot's return handler. Outside the turn, echo completion records the correlation
// independently of the next local driver call (§13). One operation: correlation registration / first send -> return handler echo
// validation. The DTO's returnSpotId names the source User SpotId. send-send; ordinary; payload 4096 bytes.
// Store: run Docker Redis. Null: physical connections, worker, Actor, fanout; Spot internals are not observable.
export class S2sSpotToChannelSendSendEchoScenario {
  private sequences: number[] = [];

  constructor(
    private readonly spots: ZLinkSpotOutbound,
    private readonly manager: ZLinkSpotManager,
    private readonly measurement: Measurement,
    private readonly config: RoleConfig,
    private readonly meshRuntime: ZLinkRouteMeshRuntime,
    private readonly readiness: ObjectsReadiness,
    private readonly metrics: ScenarioMetrics,
    private readonly correlations: SendSendCorrelation
  ) {}

  async prepare(): Promise<void> {
    const { config, measurement } = this;
    const objects = await createSpots(config, this.manager, this.meshRuntime, measurement);
    if (!objects) return;
    try {
      await until(
        () =>
          this.meshRuntime
            .snapshot(config.meshName!)
            .channels.some(
              (channel) =>
                channel.channelName === config.channelName &&
                channel.isReady &&
                channel.readyTargetCount > 0
            ),
        config.workload.setupTimeoutMs,
        'the Channel target to be ready'
      );
      this.sequences = new Array<number>(config.workload.logicalStreams as number).fill(0);
      const probes: unknown[] = [];
      for (let target = 0; target < config.spotIds.length; target++) {
        const echo = measurement
          .request(target, ++this.sequences[target % this.sequences.length], true)
          .with({ returnSpotId: config.spotIds[target] });
        const driven = await this.spots
          .requestToSpot(config.spotIds[target], new PerfDriveRequest(echo))
          .timeout(measurement.callTimeout())
          .submit<PerfDriveReply>();
        if (!driven.started) throw new Error('The setup probe was not started.');
        const entry = this.correlations.find(echo.correlationId);
        if (!entry) throw new Error('The setup probe registered no correlation.');
        const { error } = await this.correlations.completeAsync(entry);
        if (error) throw error;
        probes.push({ correlationId: echo.correlationId });
      }
      publishSpots(this.readiness, objects); // objectsReady only after every probe, so warmup never overlaps one
      measurement.setupEvidence = [
        {
          kind: 'typedProbeEcho',
          source: 'Spot sendToChannel -> Channel sendToSpot -> Spot return handler',
          observedValue: probes
        }
      ];
    } catch (error) {
      measurement.recordDiagnostic(error);
    }
  }

  run = (): Promise<void> =>
    runTerminalStreams(
      this.config.workload.logicalStreams as number,
      () => this.measurement.canIssue,
      (stream) => this.loop(stream)
    );

  private async loop(stream: number): Promise<void> {
    const { config, measurement, metrics } = this;
    const spotId = config.spotIds[stream % config.spotIds.length];
    if (!measurement.canIssue) return;
    const echo = measurement
      .request(stream, ++this.sequences[stream])
      .with({ returnSpotId: spotId });
    const driverStarted = PerfClock.now();
    metrics.count('driver.issued');
    let driven: PerfDriveReply | undefined;
    let driverError: unknown;
    let driverCompletedTicks: bigint | undefined;
    try {
      driven = await this.spots
        .requestToSpot(spotId, new PerfDriveRequest(echo))
        .timeout(measurement.callTimeout(true))
        .submit<PerfDriveReply>();
      driverCompletedTicks = PerfClock.now();
    } catch (error) {
      metrics.count('driver.failed');
      measurement.recordDiagnostic(error);
      driverError = error;
    }
    if (driven && !driven.started) {
      metrics.count('driver.notStarted');
      return;
    }
    const entry = this.correlations.find(echo.correlationId);
    if (!entry) {
      if (driven?.started)
        measurement.recordDiagnostic(new Error('The started drive registered no correlation.'));
      return;
    }
    // Remote echoes do not gate the next local driver call (§4.3).
    void this.completeEcho(entry, driverError, driven, driverStarted, driverCompletedTicks).catch(
      (error) => measurement.recordDiagnostic(error)
    );
  }

  private async completeEcho(
    entry: ReturnType<SendSendCorrelation['register']>,
    driverError: unknown,
    driven: PerfDriveReply | undefined,
    driverStarted: bigint,
    driverCompletedTicks: bigint | undefined
  ): Promise<void> {
    const { measurement, metrics } = this;
    const { error, completedTicks } = await this.correlations.completeAsync(entry);
    const operationSucceeded = measurement.completeOperation(
      entry.startedTicks,
      error,
      completedTicks
    );
    if (
      driverError === undefined &&
      driven?.started &&
      operationSucceeded &&
      driverCompletedTicks !== undefined
    )
      metrics.record('driverLatencyMs', driverStarted, driverCompletedTicks, completedTicks);
  }
}

@Injectable({ scope: Scope.TRANSIENT })
export class S2sSendSendSpot extends ActorlessSpot {}

// The source Spot handler: register the correlation, make the first public send and return when it is admitted.
@zlinkSpotPacketHandler({ spot: () => S2sSendSendSpot, packetName: 'PerfDriveRequest' })
@Injectable()
export class S2sSendDriveHandler implements ZLinkSpotRequestHandler<
  S2sSendSendSpot,
  PerfDriveRequest,
  PerfDriveReply
> {
  constructor(
    @Inject(Measurement) private readonly measurement: Measurement,
    @Inject(ScenarioMetrics) private readonly metrics: ScenarioMetrics,
    @Inject(SendSendCorrelation) private readonly correlations: SendSendCorrelation,
    @Inject(ROLE_CONFIG) private readonly config: RoleConfig
  ) {}

  async handle(
    spot: S2sSendSendSpot,
    drive: PerfDriveRequest,
    _context: ZLinkMessageContext
  ): Promise<PerfDriveReply> {
    const { measurement, metrics, correlations, config } = this;
    measurement.handlerEnter();
    try {
      let request = drive.echo;
      measurement.validateRequest(request, null, spot.context.spotId);
      if (!request.returnSpotId) throw new Error('No return SpotId in the request.');
      if (request.phase === 'measured') metrics.count('spot.applicationHandlerEntries');
      const probe = measurement.phase === 'setup'; // the setup probe is no measured operation
      let started = PerfClock.now();
      if (!probe) {
        const begun = measurement.beginOperation('send');
        if (begun === undefined) return new PerfDriveReply(false, null);
        started = begun;
      }
      request = new PerfEchoRequest({ ...request, sentTicks: started.toString() }); // decoded requests are plain objects
      try {
        const entry = correlations.register(request, started); // §13: registered right before the first public send
        try {
          await spot.context.outbound.sendToChannel(config.channelName!, request).submit();
          correlations.firstSendEnded(entry, undefined);
        } catch (error) {
          correlations.firstSendEnded(entry, error);
        }
      } catch (error) {
        // The operation started but cannot be tied to a correlation: it ends here as a failure.
        if (!probe) measurement.completeOperation(started, error);
        throw error;
      }
      return new PerfDriveReply(true, null); // send/send: the reply is only the first send's acknowledgement
    } finally {
      measurement.handlerExit();
    }
  }
}

// The return send arrives as its own Spot packet: the correlation decides the operation's first result.
@zlinkSpotPacketHandler({ spot: () => S2sSendSendSpot, packetName: 'PerfEchoReply' })
@Injectable()
export class S2sSendReturnHandler implements ZLinkSpotPacketHandler<
  S2sSendSendSpot,
  PerfEchoReply
> {
  constructor(
    @Inject(Measurement) private readonly measurement: Measurement,
    @Inject(ScenarioMetrics) private readonly metrics: ScenarioMetrics,
    @Inject(SendSendCorrelation) private readonly correlations: SendSendCorrelation
  ) {}

  async handle(
    _spot: S2sSendSendSpot,
    message: PerfEchoReply,
    _context: ZLinkMessageContext
  ): Promise<void> {
    this.measurement.handlerEnter();
    try {
      if (message.phase === 'measured') this.metrics.count('spot.applicationHandlerEntries');
      this.correlations.reply(message);
    } finally {
      this.measurement.handlerExit();
    }
  }
}

export async function runS2sSpotToChannelSendSendEcho(config: RoleConfig): Promise<void> {
  if (config.role !== 'spot' || !config.source)
    throw new Error('The Spot role is the source of this scenario.');
  const measurement = new Measurement(config, config.source);
  const metrics = new ScenarioMetrics(measurement)
    .counters(
      'driver.issued',
      'driver.notStarted',
      'driver.failed',
      'spot.applicationHandlerEntries'
    )
    .latency('driverLatencyMs', 'driver.latency')
    .spotInternalsUnsupported();
  const correlations = new SendSendCorrelation(measurement, metrics);
  const readiness = new ObjectsReadiness(false, 'This cell has not created its User Spots yet.');
  let scenario: S2sSpotToChannelSendSendEchoScenario | undefined;
  await runRole(
    {
      config,
      objects: readiness,
      providers: [
        { provide: ScenarioMetrics, useValue: metrics },
        { provide: SendSendCorrelation, useValue: correlations },
        S2sSendSendSpot,
        S2sSendDriveHandler,
        S2sSendReturnHandler
      ],
      configureFramework: (builder) => configureSpotRole(builder, config, true, S2sSendSendSpot),
      workload: () => scenario?.run,
      prepare: async (app) => {
        scenario = new S2sSpotToChannelSendSendEchoScenario(
          app.get<ZLinkSpotOutbound>(ZLINK_SPOT_OUTBOUND, { strict: false }),
          app.get<ZLinkSpotManager>(ZLINK_SPOT_MANAGER, { strict: false }),
          measurement,
          config,
          app.get<ZLinkRouteMeshRuntime>(ZLINK_ROUTE_MESH_RUNTIME, { strict: false }),
          readiness,
          metrics,
          correlations
        );
        await scenario.prepare();
      }
    },
    measurement
  );
}
