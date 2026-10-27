import { Injectable, Scope } from '@nestjs/common';
import { zlinkEntrySpotActorSendHandler } from '@zlink-systems/nestjs';
import type {
  ZLinkEntrySpot,
  ZLinkEntrySpotContext,
  ZLinkMessageContext
} from '@zlink-systems/framework';
import { PacketNames, townId } from './constants';
import { ChannelRules } from './channel-rules';
import { TownView, DungeonView, ActionRejected, JoinTownRequest } from './messages';
import { Player } from './player';
@Injectable({ scope: Scope.TRANSIENT })
export class Entry implements ZLinkEntrySpot<Player> {
  readonly context!: ZLinkEntrySpotContext<Player, Entry>;
  async onJoinedActor(_actor: Player): Promise<void> {}
  async onLeaveActor(_actor: Player): Promise<void> {}
}
@Injectable()
@zlinkEntrySpotActorSendHandler({
  entrySpot: () => Entry,
  actor: () => Player,
  packetName: PacketNames.joinTownRequest
})
export class EnterTown {
  async handle(
    _spot: Entry,
    actor: Player,
    _context: ZLinkMessageContext,
    request: JoinTownRequest
  ): Promise<void> {
    actor.assertIdentity(request.playerId);
    const reason = ChannelRules.validate(request.channel);
    if (reason !== null) {
      await actor.context.boundSession.send(new ActionRejected(reason)).submit();
      return;
    }
    actor.context
      .joinSpot(townId(request.channel), new JoinTownRequest(request.playerId, request.channel))
      .defer();
  }
}

@Injectable()
@zlinkEntrySpotActorSendHandler({
  entrySpot: () => Entry,
  actor: () => Player,
  packetName: PacketNames.townView
})
@zlinkEntrySpotActorSendHandler({
  entrySpot: () => Entry,
  actor: () => Player,
  packetName: PacketNames.dungeonView
})
@zlinkEntrySpotActorSendHandler({
  entrySpot: () => Entry,
  actor: () => Player,
  packetName: PacketNames.actionRejected
})
export class EntryNotifications {
  async handle(
    _spot: Entry,
    actor: Player,
    context: ZLinkMessageContext,
    message: TownView | DungeonView | ActionRejected
  ): Promise<void> {
    await actor.deliver(context.packetName, message);
  }
}
