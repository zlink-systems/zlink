import { Injectable } from '@nestjs/common';
import type {
  ZLinkActor,
  ZLinkActorCreateResponse,
  ZLinkActorContext,
  ZLinkEntrySpot,
  ZLinkEntrySpotContext,
  ZLinkMessage,
  ZLinkMessageContext,
  ZLinkPublishMessageContext,
  ZLinkRequestHandler,
  ZLinkSpot,
  ZLinkSpotContext,
  ZLinkSpotSubscriptionHandler,
  ZLinkSpotTimerHandler,
  ZLinkTimerTick
} from '@zlink-systems/framework';
import {
  zlinkEntrySpotSubscriptionHandler,
  zlinkSpotTimerHandler
} from '@zlink-systems/nestjs';
import { RuntimeMonitoringNames, type MonitoringPublish, type ProfileReq, type ProfileRes } from '../../../Shared/messages';
import { EvidenceStore } from '../Infrastructure/evidence-store';

@Injectable()
export class MonitoringPublishGate {
  private static readonly blocked = new Set<string>();
  private static readonly waiters = new Map<string, Set<() => void>>();

  setBlocked(target: string, blocked: boolean): void {
    if (blocked) {
      MonitoringPublishGate.blocked.add(target);
      return;
    }
    MonitoringPublishGate.blocked.delete(target);
    const pending = MonitoringPublishGate.waiters.get(target);
    MonitoringPublishGate.waiters.delete(target);
    for (const resolve of pending ?? []) resolve();
  }

  isBlocked(target: string): boolean {
    return MonitoringPublishGate.blocked.has(target);
  }

  async wait(target: string): Promise<void> {
    if (!MonitoringPublishGate.blocked.has(target)) return;
    await new Promise<void>((resolve) => {
      const pending = MonitoringPublishGate.waiters.get(target) ?? new Set<() => void>();
      pending.add(resolve);
      MonitoringPublishGate.waiters.set(target, pending);
    });
  }
}

@Injectable()
export class ProfileRequestHandler implements ZLinkRequestHandler<ProfileReq, ProfileRes> {
  constructor(private readonly evidence: EvidenceStore) {}

  async handle(request: ProfileReq, context: ZLinkMessageContext): Promise<ProfileRes> {
    this.evidence.add(
      `profile-request|rid=${this.evidence.rid}|marker=${request.marker}`
      + `|value=${request.value}|packet=${context.packetName}`
    );
    return { value: `profile:${request.value}`, providerRid: this.evidence.rid, marker: request.marker };
  }
}

export class MonitoringActor implements ZLinkActor {
  constructor(
    readonly actorId: string,
    readonly context: ZLinkActorContext
  ) {}
}

export class MonitoringActorFactory {
  async create(context: ZLinkActorContext): Promise<MonitoringActor> {
    return new MonitoringActor(context.actorId, context);
  }
}

@Injectable()
@zlinkEntrySpotSubscriptionHandler({
  entrySpot: () => MonitoringEntrySpot,
  channelName: RuntimeMonitoringNames.spotChannel,
  topic: RuntimeMonitoringNames.publishTopic
})
export class MonitoringEntryPublishHandler
  implements ZLinkSpotSubscriptionHandler<MonitoringEntrySpot, MonitoringPublish> {
  constructor(
    private readonly evidence: EvidenceStore,
    private readonly gate: MonitoringPublishGate
  ) {}

  async handle(
    spot: MonitoringEntrySpot,
    event: MonitoringPublish,
    context: ZLinkPublishMessageContext
  ): Promise<void> {
    void context;
    await this.gate.wait(`entry:${this.evidence.rid}`);
    this.evidence.add(
      `publish-received|rid=${this.evidence.rid}|spot=entry|marker=${event.marker}`
    );
    void spot;
  }
}

export class MonitoringUserSpot implements ZLinkSpot<MonitoringActor> {
  readonly context!: ZLinkSpotContext<MonitoringActor, MonitoringUserSpot>;

  configure(): void {
    this.context.handlers.addSubscribe(
      MonitoringUserSpotPublishHandler,
      RuntimeMonitoringNames.spotChannel,
      RuntimeMonitoringNames.publishTopic
    );
    this.requireEvidence().add(
      `subscription-configured|rid=${this.requireEvidence().rid}|spot=${this.context.spotId}`
    );
  }

  async onInitialize(): Promise<void> {
    this.requireEvidence().add(`spot-ready|rid=${this.requireEvidence().rid}|spot=${this.context.spotId}`);
  }

  async onActorJoin(_actorId: string, _request: ZLinkMessage): Promise<{ accepted: boolean }> {
    return { accepted: true };
  }

  async onJoinedActor(_actor: MonitoringActor): Promise<void> {}

  async onLeaveActor(_actor: MonitoringActor): Promise<void> {}

  async onDisconnectActor(_actor: MonitoringActor): Promise<void> {}

  private requireEvidence(): EvidenceStore {
    const evidence = MonitoringUserSpot.evidence;
    if (evidence === undefined) throw new Error('MonitoringUserSpot evidence is not configured.');
    return evidence;
  }

  private static evidence?: EvidenceStore;

  static useEvidence(evidence: EvidenceStore): void {
    MonitoringUserSpot.evidence = evidence;
  }
}

@Injectable()
export class MonitoringUserSpotPublishHandler
  implements ZLinkSpotSubscriptionHandler<MonitoringUserSpot, MonitoringPublish> {
  constructor(
    private readonly evidence: EvidenceStore,
    private readonly gate: MonitoringPublishGate
  ) {}

  async handle(
    spot: MonitoringUserSpot,
    event: MonitoringPublish,
    context: ZLinkPublishMessageContext
  ): Promise<void> {
    void context;
    const spotId = String(spot.context.spotId);
    const payloadBytes = event.blocker?.length ?? 0;
    this.evidence.add(
      `publish-entered|rid=${this.evidence.rid}|spot=${spotId}|marker=${event.marker}|payloadBytes=${payloadBytes}`
    );
    await this.gate.wait(`spot:${spotId}`);
    this.evidence.add(
      `publish-received|rid=${this.evidence.rid}|spot=${spotId}|marker=${event.marker}|payloadBytes=${payloadBytes}`
    );
  }
}

@Injectable()
export class MonitoringEntrySpot implements ZLinkEntrySpot<MonitoringActor> {
  declare readonly context: ZLinkEntrySpotContext<MonitoringActor>;

  async onJoinedActor(_actor: ZLinkActor): Promise<void> {}

  async onLeaveActor(_actor: ZLinkActor): Promise<void> {}

  async onDisconnectActor(_actor: ZLinkActor): Promise<void> {}

  async onInitialize(): Promise<void> {
    await this.context.addTimer('failing', 1000, FailingTimerHandler, { stopOnUnhandledException: false });
    await this.context.addTimer('stopping', 1000, FailingTimerHandler, { stopOnUnhandledException: true });
  }

  async onCreateActor(_actor: MonitoringActor, _createRequest: ZLinkMessage): Promise<ZLinkActorCreateResponse> {
    return { accepted: true };
  }
}

@Injectable()
@zlinkSpotTimerHandler()
export class FailingTimerHandler implements ZLinkSpotTimerHandler<ZLinkEntrySpot> {
  private readonly failedTimers = new Set<string>();

  async handle(_spot: ZLinkEntrySpot, tick: ZLinkTimerTick): Promise<void> {
    if (this.failedTimers.has(tick.name)) return;
    this.failedTimers.add(tick.name);
    throw new Error('monitoring timer failure');
  }
}
