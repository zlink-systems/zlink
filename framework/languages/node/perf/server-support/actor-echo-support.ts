import { Inject, Injectable, Scope } from '@nestjs/common';
import {
  ZLINK_ACTOR_MANAGER, ZLINK_ROUTE_CLIENT, ZLINK_ROUTE_MESH_RUNTIME, zlinkEntrySpotActorRequestHandler, zlinkEntrySpotActorSendHandler
} from '@zlink-systems/nestjs';
import type {
  ZLinkActor, ZLinkActorContext, ZLinkActorFactory, ZLinkActorManager, ZLinkEntrySpot, ZLinkEntrySpotContext, ZLinkMessage, ZLinkMessageContext,
  ZLinkRouteClient, ZLinkRouteMeshRuntime, ZLinkSession, ZLinkSessionActor, ZLinkSessionContext, ZLinkSessionDispatchContext, ZLinkSessionFactory, ZLinkStreamError
} from '@zlink-systems/framework';
import { PerfClock } from '../shared/clock';
import { PerfEchoReply, PerfEchoRequest, PerfValidationException, RoleConfig } from '../shared/contracts';
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
@zlinkEntrySpotActorRequestHandler({ entrySpot: () => PerfEntrySpot, actor: () => PerfActor, packetName: 'PerfEchoRequest' })
@Injectable()
export class ActorEchoRequestHandler {
  constructor(@Inject(Measurement) private readonly measurement: Measurement) {}

  async handle(_spot: PerfEntrySpot, _actor: PerfActor, _context: ZLinkMessageContext, request: PerfEchoRequest): Promise<PerfEchoReply> {
    const received = PerfClock.now();
    const measurement = this.measurement;
    measurement.handlerEnter();
    try {
      measurement.validateRequest(request);
      const reply = PayloadPattern.reply(request, received);
      measurement.recordReply(request);
      if (measurement.phase === 'setup') measurement.setupEvidence = [{ kind: 'typedProbeReply', source: 'ZLinkEntrySpotActorRequestHandler<PerfEntrySpot,PerfActor,PerfEchoRequest,PerfEchoReply>', observedValue: request.correlationId }];
      return reply;
    } catch (error) {
      measurement.recordDiagnostic(error);
      throw error;
    } finally {
      measurement.handlerExit();
    }
  }
}

@zlinkEntrySpotActorSendHandler({ entrySpot: () => PerfEntrySpot, actor: () => PerfActor, packetName: 'PerfEchoRequest' })
@Injectable()
export class ActorEchoSendHandler {
  constructor(
    @Inject(Measurement) private readonly measurement: Measurement,
    @Inject(ROLE_CONFIG) private readonly config: RoleConfig,
    @Inject(ZLINK_ROUTE_CLIENT) private readonly route: ZLinkRouteClient
  ) {}

  async handle(_spot: PerfEntrySpot, _actor: PerfActor, _context: ZLinkMessageContext, request: PerfEchoRequest): Promise<void> {
    const received = PerfClock.now();
    const measurement = this.measurement;
    measurement.handlerEnter();
    try {
      measurement.validateRequest(request, this.config.channelName);
      const reply = PayloadPattern.reply(request, received);
      measurement.recordApplicationCall(request, 'send');
      await this.route.sendToChannel(request.returnChannel!, reply).submit();
      if (measurement.phase === 'setup') measurement.setupEvidence = [{ kind: 'typedProbeReply', source: 'ZLinkRouteClient.sendToChannel(returnChannel).submit', observedValue: request.correlationId }];
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
export function watchActorPlacement(config: RoleConfig, measurement: Measurement, readiness: ObjectsReadiness, mesh: ZLinkRouteMeshRuntime): void {
  const timer = setInterval(() => {
    if (measurement.phase !== 'setup') { clearInterval(timer); return; }
    const placement = mesh.snapshot(config.meshName!).placement;
    readiness.set(placement.isAvailable && placement.activeActorCount > 0, 'No Actor is active on this Object Server.',
      [{ kind: 'actorPlacement', source: 'ZLinkRouteMeshRuntime.snapshot.placement', observedValue: { isAvailable: placement.isAvailable,
        activeActorCount: placement.activeActorCount, expectedActors: config.actorIds.length } }]);
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
  // Public status shows when the Actor node is a ready peer; every session shares this one wait, and the create
  // itself is never retried.
  private peer: Promise<void> | undefined;

  constructor(
    @Inject(ROLE_CONFIG) private readonly config: RoleConfig,
    @Inject(Measurement) private readonly measurement: Measurement,
    @Inject(ObjectsReadiness) private readonly readiness: ObjectsReadiness,
    @Inject(ZLINK_ACTOR_MANAGER) private readonly actors: ZLinkActorManager,
    @Inject(ZLINK_ROUTE_MESH_RUNTIME) private readonly mesh: ZLinkRouteMeshRuntime
  ) {}

  private waitForPeer(): Promise<void> {
    this.peer ??= (async () => {
      const deadline = Date.now() + this.config.workload.setupTimeoutMs;
      while (this.config.objectRole === 'ObjectClient' && this.mesh.snapshot(this.config.meshName!).readyPeerCount === 0) {
        if (Date.now() >= deadline) throw new Error('No ready Actor peer inside setupTimeoutMs.');
        await new Promise((resolve) => setTimeout(resolve, 10));
      }
    })();
    return this.peer;
  }

  async prepare(session: ZLinkSessionContext, probe: ZLinkMessage): Promise<ZLinkSessionActor> {
    if (this.measurement.phase !== 'setup') throw new Error('Actors are created and bound during setup only.');
    try {
      const request = probe.decode(PerfEchoRequest);
      if (!(request.clientId >= 0 && request.clientId < this.config.actorIds.length)) throw new PerfValidationException('IdentityMismatch', 'clientId has no Actor ID in this cell.');
      const signal = AbortSignal.timeout(this.config.workload.setupTimeoutMs);
      await this.waitForPeer();
      const createStarted = PerfClock.now();
      const result = await this.actors.getOrCreate(this.config.actorIds[request.clientId], PERF_ACTOR_TYPE)
        .inMesh(this.config.meshName!).timeout(this.config.workload.setupTimeoutMs).submit(signal);
      if (result.status === 'rejected') throw new Error('Actor creation was rejected.');
      const bindStarted = PerfClock.now();
      const binding = await session.actors.bindOrGet(result.actor, signal);
      const done = PerfClock.now();
      this.record(result.status === 'created', bindStarted - createStarted, done - bindStarted);
      return binding;
    } catch (error) {
      this.measurement.recordDiagnostic(error);
      this.failed++;
      this.publish();
      throw error;
    }
  }

  private record(wasCreated: boolean, create: bigint, bind: bigint): void {
    if (wasCreated) this.created++; else this.existing++;
    this.bound++;
    this.createNs += create; if (create > this.createMaxNs) this.createMaxNs = create;
    this.bindNs += bind; if (bind > this.bindMaxNs) this.bindMaxNs = bind;
    this.publish();
  }

  private publish(): void {
    const bound = this.bound;
    this.readiness.set(bound > 0 && this.failed === 0, this.failed > 0 ? 'Actor create or bind failed.' : 'No Actor is bound to a session yet.',
      [{ kind: 'actorCreateAndBind', source: 'ZLinkActorManager.getOrCreate + ZLinkSessionActors.bindOrGet',
        observedValue: { created: this.created, existing: this.existing, bound, failed: this.failed, expectedActors: this.config.actorIds.length,
          createMeanMs: bound === 0 ? 0 : Number(this.createNs) / 1e6 / bound, createMaxMs: Number(this.createMaxNs) / 1e6,
          bindMeanMs: bound === 0 ? 0 : Number(this.bindNs) / 1e6 / bound, bindMaxMs: Number(this.bindMaxNs) / 1e6 } }]);
    // The Session role has no typed reply of its own: its setup probe is the admitted relay of a bound Actor.
    if (bound > 0) this.measurement.setupEvidence = [{ kind: 'relayAdmission', source: 'ZLinkSessionActor.relay', observedValue: { bound } }];
  }
}

// §10.1/§10.2: the session relays every packet to the Actor bound to it; the Actor handler's return value is
// the reply of the original STREAM request (Session binding §5), so the session writes no reply.
class PerfActorRelaySession implements ZLinkSession {
  private binding: ZLinkSessionActor | undefined;
  constructor(readonly context: ZLinkSessionContext, private readonly measurement: Measurement, private readonly setup: SessionActorSetup) {}

  async onError(_context: ZLinkSessionContext, error: ZLinkStreamError): Promise<void> {
    this.measurement.recordDiagnostic(new Error(`STREAM ${error.error}: ${error.message}`));
  }

  async onDispatch(dispatch: ZLinkSessionDispatchContext, payload: ZLinkMessage): Promise<void> {
    const actor = dispatch.actor ?? this.binding ?? (this.binding = await this.setup.prepare(this.context, payload));
    try {
      await actor.relay(dispatch, payload);
    } catch (error) {
      this.measurement.recordDiagnostic(error);
      throw error;
    }
  }
}

@Injectable()
export class PerfActorRelaySessionFactory implements ZLinkSessionFactory<PerfActorRelaySession> {
  constructor(@Inject(Measurement) private readonly measurement: Measurement, @Inject(SessionActorSetup) private readonly setup: SessionActorSetup) {}

  async create(context: ZLinkSessionContext): Promise<PerfActorRelaySession> {
    return new PerfActorRelaySession(context, this.measurement, this.setup);
  }
}
