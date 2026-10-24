import { Inject, Injectable, Scope } from '@nestjs/common';
import { ZLINK_ACTOR_CLIENT } from '@zlink-systems/nestjs';
import type {
  ZLinkActorClient,
  ZLinkEntrySpot,
  ZLinkEntrySpotContext,
  ZLinkMessage,
  ZLinkSpot,
  ZLinkSpotContext,
  ZLinkSpotRelocationAdapter
} from '@zlink-systems/framework';
import { Names, Game, zoneId, adjacentZones, borderTopic } from '../shared/constants';
import { BorderPresence, BorderVisible, EnterZone } from '../shared/messages';
import { Player } from './player';
import { BorderHandler, BorderTick } from './handlers';
@Injectable({ scope: Scope.TRANSIENT })
export class Entry implements ZLinkEntrySpot<Player> {
  readonly context!: ZLinkEntrySpotContext<Player, Entry>;
  async onJoinedActor(_actor: Player): Promise<void> {}
  async onLeaveActor(_actor: Player): Promise<void> {}
}
@Injectable({ scope: Scope.TRANSIENT })
export class Zone implements ZLinkSpot<Player> {
  readonly context!: ZLinkSpotContext<Player, Zone>;
  // Visibility is a projection; Player owns the authoritative movement state.
  private readonly visible = new Map<string, EnterZone>();
  private readonly arrivals = new Map<string, EnterZone>();
  constructor(@Inject(ZLINK_ACTOR_CLIENT) private readonly actors: ZLinkActorClient) {}
  configure(): void {
    this.context.handlers.addTimerHandler(BorderTick);
    for (const adjacent of adjacentZones(String(this.context.spotId)))
      this.context.handlers.addSubscribe(
        BorderHandler,
        Names.border,
        borderTopic(adjacent, String(this.context.spotId))
      );
  }
  async onInitialize(): Promise<void> {
    await this.context.addTimer(Names.borderTimer, Game.tickMs, BorderTick);
  }
  async onActorJoin(actorId: string, request: ZLinkMessage) {
    const arrival = request.decode(EnterZone);
    if (zoneId(arrival.x) !== String(this.context.spotId)) return { accepted: false };
    this.arrivals.set(actorId, arrival);
    return { accepted: true };
  }
  async onJoinedActor(actor: Player): Promise<void> {
    const arrival = this.arrivals.get(actor.context.actorId);
    if (!arrival) throw new Error('Zone admission has no movement intent');
    this.arrivals.delete(actor.context.actorId);
    actor.apply(arrival);
    this.remember(actor);
  }
  async onLeaveActor(actor: Player): Promise<void> {
    this.visible.delete(actor.context.actorId);
  }
  async onDisconnectActor(actor: Player): Promise<void> {
    await this.onLeaveActor(actor);
  }
  remember(actor: Player): void {
    this.visible.set(actor.context.actorId, new EnterZone(actor.x, actor.sequence));
  }
  async publishBorder(): Promise<void> {
    const id = String(this.context.spotId);
    const adjacent = adjacentZones(id);
    for (const [actorId, position] of this.visible) {
      const offset = position.x % Game.zoneWidth;
      if (offset >= Game.borderRadius && offset < Game.zoneWidth - Game.borderRadius) continue;
      for (const target of adjacent)
        await this.context.outbound
          .publish(
            Names.border,
            borderTopic(id, target),
            new BorderPresence(id, actorId, position.x, position.sequence)
          )
          .submit();
    }
  }
  async showBorder(presence: BorderPresence): Promise<void> {
    for (const [observerId, observer] of this.visible) {
      if (Math.abs(observer.x - presence.x) > Game.borderRadius) continue;
      await this.actors
        .sendToActor(
          observerId,
          new BorderVisible(
            String(this.context.spotId),
            presence.sourceZoneId,
            presence.actorId,
            presence.x,
            presence.sequence
          )
        )
        .submit();
    }
  }
  async deliver(actor: Player, presence: BorderVisible): Promise<void> {
    // Membership or visibility may change before a queued notification is handled.
    if (
      presence.observerZoneId !== actor.context.spotId ||
      !this.visible.has(actor.context.actorId)
    )
      return;
    await actor.context.boundSession.send(presence).submit();
  }
  capture(): ZoneSnapshot {
    return { visible: [...this.visible], arrivals: [...this.arrivals] };
  }
  restore(snapshot: ZoneSnapshot): void {
    this.visible.clear();
    this.arrivals.clear();
    for (const [id, p] of snapshot.visible) this.visible.set(id, p);
    for (const [id, p] of snapshot.arrivals) this.arrivals.set(id, p);
  }
}
interface ZoneSnapshot {
  visible: [string, EnterZone][];
  arrivals: [string, EnterZone][];
}
@Injectable()
export class ZoneRelocation implements ZLinkSpotRelocationAdapter<Zone> {
  async capture(spot: Zone): Promise<Uint8Array> {
    return Buffer.from(JSON.stringify(spot.capture()));
  }
  async restore(spot: Zone, payload: Uint8Array): Promise<void> {
    spot.restore(JSON.parse(Buffer.from(payload).toString('utf8')) as ZoneSnapshot);
  }
}
