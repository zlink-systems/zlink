import { zlinkSpotActorSendHandler } from '@zlink-systems/nestjs';
import { ZoneSpot } from '../Spots/zone-spot';
import type {
  ZLinkActor,
  ZLinkActorContext,
  ZLinkActorJoinCompletion,
  ZLinkMessageContext
} from '@zlink-systems/framework';
import { ZLinkFrameworkErrorKind } from '@zlink-systems/framework';
import { ZoneIds } from '../../../../../Shared/spec';
import {
  EnterZoneRes,
  JoinWorldRes,
  MoveRejectedNotify,
  WorldAnnounceNotify,
  ZoneChangedNotify,
  ZoneStateNotify
} from '../../../../../Shared/contracts';
import type { ZoneId } from '../../../../../Shared/spec';

type PendingJoinKind = 'world' | 'move' | 'bot';

class PlayerActor implements ZLinkActor {
  readonly context!: ZLinkActorContext;

  constructor(
    readonly actorId: string,
    public x = 25,
    public y = 25,
    public zoneId: ZoneId = ZoneIds.northWest,
    public isBot = false,
    public dirX = 0,
    public dirY = 0
  ) {}

  private pendingJoinKind: PendingJoinKind | null = null;

  get hasPendingJoin(): boolean {
    return this.pendingJoinKind !== null;
  }

  beginPendingJoin(kind: PendingJoinKind): void {
    if (this.pendingJoinKind !== null) {
      throw new Error(`Actor '${this.actorId}' already has a pending zone join.`);
    }
    this.pendingJoinKind = kind;
  }

  pendingJoin(): PendingJoinKind | null {
    return this.pendingJoinKind;
  }

  restorePendingJoin(kind: PendingJoinKind | null): void {
    this.pendingJoinKind = kind;
  }

  completePendingJoin(): void {
    this.pendingJoinKind = null;
  }

  push(payload: unknown): void {
    if (this.isBot) return;
    const packetName = typeof payload === 'object' && payload !== null
      ? payload.constructor.name
      : typeof payload;
    void this.context.boundSession.send(payload).submit().then(
      () => undefined,
      (error: unknown) => console.error(
        `actor push failed actor=${this.actorId} packet=${packetName}`,
        error instanceof Error ? error.message : String(error)
      )
    );
  }

  async onJoinCompleted(completion: ZLinkActorJoinCompletion): Promise<void> {
    const kind = 'kind' in completion ? completion.kind : 'none';
    console.log(
      `actor join completed actor=${this.actorId} status=${completion.status}`
        + ` kind=${kind}`
    );
    const pending = this.pendingJoinKind;
    this.completePendingJoin();
    if (completion.status === 'accepted') {
      if (pending === 'world') await this.sendJoinResult(null);
      return;
    }
    if (pending === 'bot') {
      this.dirX *= -1;
      this.dirY *= -1;
      return;
    }
    let reason = 'ZoneMaintenance';
    if (completion.status === 'rejected' && completion.reply !== undefined) {
      reason = completion.reply.decode<EnterZoneRes>().error ?? reason;
    } else if (completion.status === 'failed') {
      reason = ZLinkFrameworkErrorKind[completion.kind];
    }
    if (pending === 'world') {
      await this.sendJoinResult(reason);
      return;
    }
    this.push(new MoveRejectedNotify(reason as never, this.x, this.y));
  }

  async sendJoinResult(error: string | null): Promise<void> {
    await this.context.boundSession
      .send(new JoinWorldRes(this.actorId, this.zoneId, this.x, this.y, error))
      .submit();
  }
}

class DeliverZoneNotificationMsg {
  readonly packetName: string;

  constructor(readonly payload: unknown) {
    this.packetName = typeof payload === 'object' && payload !== null
      ? payload.constructor.name
      : '';
  }
}

@zlinkSpotActorSendHandler({
  spot: () => ZoneSpot,
  actor: () => PlayerActor,
  packetName: 'DeliverZoneNotificationMsg'
})
class DeliverZoneNotificationMsgHandler {
  async handle(_spot: ZoneSpot, actor: PlayerActor, _context: ZLinkMessageContext, message: DeliverZoneNotificationMsg): Promise<void> {
    const value = message.payload as Record<string, unknown>;
    switch (message.packetName) {
      case 'ZoneStateNotify':
        actor.push(new ZoneStateNotify(
          value.zoneId as string,
          value.tick as number,
          value.players as ConstructorParameters<typeof ZoneStateNotify>[2]
        ));
        return;
      case 'ZoneChangedNotify':
        actor.push(new ZoneChangedNotify(
          value.playerId as string,
          value.zoneId as string
        ));
        return;
      case 'WorldAnnounceNotify':
        actor.push(new WorldAnnounceNotify(value.announcementId as string, value.text as string));
        return;
      default:
        throw new Error(`Unsupported ZoneWorld notification '${message.packetName}'.`);
    }
  }
}

export { DeliverZoneNotificationMsg, DeliverZoneNotificationMsgHandler, PlayerActor };
