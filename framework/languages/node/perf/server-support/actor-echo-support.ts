import { Inject, Injectable, Scope } from '@nestjs/common';
import {
  ZLINK_ACTOR_MANAGER,
  ZLINK_ROUTE_CLIENT,
  zlinkEntrySpotActorRequestHandler,
  zlinkEntrySpotActorSendHandler
} from '@zlink-systems/nestjs';
import type {
  ZLinkActor,
  ZLinkActorContext,
  ZLinkActorFactory,
  ZLinkActorManager,
  ZLinkEntrySpot,
  ZLinkEntrySpotContext,
  ZLinkMessage,
  ZLinkMessageContext,
  ZLinkRouteClient,
  ZLinkRouteMeshRuntime,
  ZLinkSession,
  ZLinkSessionActor,
  ZLinkSessionContext,
  ZLinkSessionDispatchContext,
  ZLinkSessionFactory,
  ZLinkStreamError
} from '@zlink-systems/framework';
import { PerfClock } from '../shared/clock';
import {
  PerfEchoReply,
  PerfEchoRequest,
  PerfValidationException,
  RoleConfig
} from '../shared/contracts';
import { Measurement } from '../shared/measurement';
import { PayloadPattern } from '../shared/payload';
import { ObjectsReadiness, ROLE_CONFIG } from './server-application';

// Actor echo objects shared by the CS (§10.1, §10.2) and Actor (§10.9, §10.10) Object Servers.
// The Actor holds no state: every measured call is the typed echo of the Entry Spot's Actor handler.
export const PERF_ACTOR_TYPE = 'perf-actor';

export class PerfActor implements ZLinkActor {
  constructor(readonly context: ZLinkActorContext) {}
}

@Injectable()
export class PerfActorFactory implements ZLinkActorFactory<PerfActor> {
  async create(context: ZLinkActorContext): Promise<PerfActor> {
    return new PerfActor(context);
  }
}

// Actors are created as Entry Spot members and are never moved (perf never measures relocation).
@Injectable({ scope: Scope.TRANSIENT })
export class PerfEntrySpot implements ZLinkEntrySpot<PerfActor> {
  readonly context!: ZLinkEntrySpotContext<PerfActor>;
  async onJoinedActor(_actor: PerfActor): Promise<void> {}
  async onLeaveActor(_actor: PerfActor): Promise<void> {}
}

// The request cells answer with the typed reply; the send-send cell answers with a public Channel send (§10.10).
@zlinkEntrySpotActorRequestHandler({
  entrySpot: () => PerfEntrySpot,
  actor: () => PerfActor,
  packetName: 'PerfEchoRequest'
})
@Injectable()
export class ActorEchoRequestHandler {
  constructor(@Inject(Measurement) private readonly measurement: Measurement) {}

  async handle(
    _spot: PerfEntrySpot,
    _actor: PerfActor,
    _context: ZLinkMessageContext,
    request: PerfEchoRequest
  ): Promise<PerfEchoReply> {
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
            source:
              'ZLinkEntrySpotActorRequestHandler<PerfEntrySpot,PerfActor,PerfEchoRequest,PerfEchoReply>',
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

@zlinkEntrySpotActorSendHandler({
  entrySpot: () => PerfEntrySpot,
  actor: () => PerfActor,
  packetName: 'PerfEchoRequest'
})
@Injectable()
export class ActorEchoSendHandler {
  constructor(
    @Inject(Measurement) private readonly measurement: Measurement,
    @Inject(ROLE_CONFIG) private readonly config: RoleConfig,
    @Inject(ZLINK_ROUTE_CLIENT) private readonly route: ZLinkRouteClient
  ) {}

  async handle(
    _spot: PerfEntrySpot,
    _actor: PerfActor,
    _context: ZLinkMessageContext,
    request: PerfEchoRequest
  ): Promise<void> {
    const received = PerfClock.now();
    const measurement = this.measurement;
    measurement.handlerEnter();
    try {
      measurement.validateRequest(request, this.config.channelName);
      const reply = PayloadPattern.reply(request, received);
      measurement.recordApplicationCall(request, 'send');
      await this.route.sendToChannel(request.returnChannel!, reply).submit();
      if (measurement.phase === 'setup')
        measurement.setupEvidence = [
          {
            kind: 'typedProbeReply',
            source: 'ZLinkRouteClient.sendToChannel(returnChannel).submit',
            observedValue: request.correlationId
          }
        ];
    } catch (error) {
      measurement.recordDiagnostic(error);
      throw error;
    } finally {
      measurement.handlerExit();
    }
  }
}

// The Actor role's objectsReady (§16.1): the Actors this process hosts, read from the public RouteMesh placement
// status by a background poll during setup, never from inside a handler turn.
export function watchActorPlacement(
  config: RoleConfig,
  measurement: Measurement,
  readiness: ObjectsReadiness,
  mesh: ZLinkRouteMeshRuntime
): void {
  const timer = setInterval(() => {
    if (measurement.phase !== 'setup') {
      clearInterval(timer);
      return;
    }
    const placement = mesh.snapshot(config.meshName!).placement;
    const expectedActors = config.actorIds.length;
    const ready = placement.isAvailable && placement.activeActorCount === expectedActors;
    readiness.set(
      ready,
      `${placement.activeActorCount} of ${expectedActors} expected Actors are active on this Object Server.`,
      [
        {
          kind: 'actorPlacement',
          source: 'ZLinkRouteMeshRuntime.snapshot.placement',
          observedValue: {
            isAvailable: placement.isAvailable,
            activeActorCount: placement.activeActorCount,
            expectedActors
          }
        }
      ]
    );
  }, 100);
}

// The Session role's create and bind (§10.1, §10.2 preparation): the connector's setup probe names its clientId,
// which selects the Actor ID of that connector; the Actor is created through the public manager and bound to the
// session before the probe itself is relayed. Setup latencies are kept apart from the measured operations.
@Injectable()
export class SessionActorSetup {
  private created = 0;
  private existing = 0;
  private bound = 0;
  private failed = 0;
  private createNs = 0n;
  private createMaxNs = 0n;
  private bindNs = 0n;
  private bindMaxNs = 0n;
  private evidence: unknown = null;

  constructor(
    @Inject(ROLE_CONFIG) private readonly config: RoleConfig,
    @Inject(Measurement) private readonly measurement: Measurement,
    @Inject(ZLINK_ACTOR_MANAGER) private readonly actors: ZLinkActorManager,
    @Inject(ObjectsReadiness) private readonly readiness: ObjectsReadiness
  ) {}

  async prepare(session: ZLinkSessionContext, probe: ZLinkMessage): Promise<ZLinkSessionActor> {
    if (this.measurement.phase !== 'setup')
      throw new Error('Actors are created and bound during setup only.');
    try {
      const request = probe.decode(PerfEchoRequest);
      if (!(request.clientId >= 0 && request.clientId < this.config.actorIds.length))
        throw new PerfValidationException(
          'IdentityMismatch',
          'clientId has no Actor ID in this cell.'
        );
      const signal = AbortSignal.timeout(this.config.workload.setupTimeoutMs);
      const createStarted = PerfClock.now();
      const result = await this.actors
        .getOrCreate(this.config.actorIds[request.clientId], PERF_ACTOR_TYPE)
        .inMesh(this.config.meshName!)
        .timeout(this.config.workload.setupTimeoutMs)
        .submit(signal);
      if (result.status === 'rejected') throw new Error('Actor creation was rejected.');
      const bindStarted = PerfClock.now();
      const binding = await session.actors.bindOrGet(result.actor, signal);
      const done = PerfClock.now();
      this.record(result.status === 'created', bindStarted - createStarted, done - bindStarted);
      return binding;
    } catch (error) {
      this.measurement.recordDiagnostic(error);
      this.failed++;
      this.recordEvidence();
      throw error;
    }
  }

  private record(wasCreated: boolean, create: bigint, bind: bigint): void {
    if (wasCreated) this.created++;
    else this.existing++;
    this.bound++;
    this.createNs += create;
    if (create > this.createMaxNs) this.createMaxNs = create;
    this.bindNs += bind;
    if (bind > this.bindMaxNs) this.bindMaxNs = bind;
    this.recordEvidence();
  }

  private recordEvidence(): void {
    const bound = this.bound;
    this.evidence = {
      kind: 'actorCreateAndBind',
      source: 'ZLinkActorManager.getOrCreate + ZLinkSessionActors.bindOrGet',
      observedValue: {
        created: this.created,
        existing: this.existing,
        bound,
        failed: this.failed,
        expectedActors: this.config.actorIds.length,
        createMeanMs: bound === 0 ? 0 : Number(this.createNs) / 1e6 / bound,
        createMaxMs: Number(this.createMaxNs) / 1e6,
        bindMeanMs: bound === 0 ? 0 : Number(this.bindNs) / 1e6 / bound,
        bindMaxMs: Number(this.bindMaxNs) / 1e6
      }
    };
    this.measurement.preparationEvidence.actorCreateAndBind = this.evidence;
    const expected = this.config.actorIds.length;
    this.readiness.set(
      bound === expected,
      `${bound} of ${expected} expected Actors are bound to a session.`,
      [this.evidence]
    );
  }
}

// §10.1/§10.2: the session relays every packet to the Actor bound to it; the Actor handler's return value is
// the reply of the original STREAM request (Session binding §5), so the session writes no reply.
class PerfActorRelaySession implements ZLinkSession {
  private binding: ZLinkSessionActor | undefined;
  constructor(
    readonly context: ZLinkSessionContext,
    private readonly measurement: Measurement,
    private readonly setup: SessionActorSetup
  ) {}

  async onError(_context: ZLinkSessionContext, error: ZLinkStreamError): Promise<void> {
    this.measurement.recordDiagnostic(new Error(`STREAM ${error.error}: ${error.message}`));
  }

  async onDispatch(dispatch: ZLinkSessionDispatchContext, payload: ZLinkMessage): Promise<void> {
    const actor =
      dispatch.actor ??
      this.binding ??
      (this.binding = await this.setup.prepare(this.context, payload));
    try {
      await actor.relay(dispatch, payload);
      if (this.measurement.phase === 'setup')
        this.measurement.setupEvidence = [
          { kind: 'relayAdmission', source: 'ZLinkSessionActor.relay', observedValue: true }
        ];
    } catch (error) {
      this.measurement.recordDiagnostic(error);
      throw error;
    }
  }
}

@Injectable()
export class PerfActorRelaySessionFactory implements ZLinkSessionFactory<PerfActorRelaySession> {
  constructor(
    @Inject(Measurement) private readonly measurement: Measurement,
    @Inject(SessionActorSetup) private readonly setup: SessionActorSetup
  ) {}

  async create(context: ZLinkSessionContext): Promise<PerfActorRelaySession> {
    return new PerfActorRelaySession(context, this.measurement, this.setup);
  }
}
