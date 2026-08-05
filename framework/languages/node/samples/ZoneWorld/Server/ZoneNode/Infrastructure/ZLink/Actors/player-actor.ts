import { zlinkSpotActorSendHandler } from '@zlink-systems/nestjs';
import { ZoneSpot } from '../Spots/zone-spot';
import type {
  ZLinkActor,
  ZLinkActorContext,
  ZLinkActorJoinCompletion,
  ZLinkMessageContext
} from '@zlink-systems/framework';
import { ZoneIds } from '../../../../../Shared/spec';
import {
  WorldAnnounceNotify,
  ZoneChangedNotify,
  ZoneStateNotify
} from '../../../../../Shared/contracts';
import type { ZoneId } from '../../../../../Shared/spec';

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

  private pendingJoin = false;

  get hasPendingJoin(): boolean {
    return this.pendingJoin;
  }

  beginPendingJoin(): void {
    if (this.pendingJoin) {
      throw new Error(`Actor '${this.actorId}' already has a pending zone join.`);
    }
    this.pendingJoin = true;
  }

  completePendingJoin(): void {
    this.pendingJoin = false;
  }

  push(payload: unknown): void {
    if (this.isBot) return;
    const packetName = typeof payload === 'object' && payload !== null
      ? payload.constructor.name
      : typeof payload;
    void this.context.boundSession.send(payload).submit().then(
      () => console.log(`actor push submitted actor=${this.actorId} packet=${packetName}`),
      (error: unknown) => console.error(
        `actor push failed actor=${this.actorId} packet=${packetName}`,
        error instanceof Error ? error.message : String(error)
      )
    );
  }

  async onJoinCompleted(completion: ZLinkActorJoinCompletion): Promise<void> {
    this.completePendingJoin();
    const kind = 'kind' in completion ? completion.kind : 'none';
    console.log(
      `actor join completed actor=${this.actorId} status=${completion.status}`
        + ` kind=${kind}`
    );
  }
}

class DeliverZoneNotification {
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
  packetName: 'DeliverZoneNotification'
})
class DeliverZoneNotificationHandler {
  async handle(_spot: ZoneSpot, actor: PlayerActor, _context: ZLinkMessageContext, message: DeliverZoneNotification): Promise<void> {
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
          value.zoneId as string,
          value.nodeId as string,
          value.transferred as boolean
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

export { DeliverZoneNotification, DeliverZoneNotificationHandler, PlayerActor };
