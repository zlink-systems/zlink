import { Injectable, Scope } from '@nestjs/common';
import { zlinkEntrySpotActorSendHandler } from '@zlink-systems/nestjs';
import type {
  ZLinkEntrySpot,
  ZLinkEntrySpotContext,
  ZLinkMessageContext
} from '@zlink-systems/framework';
import { PacketNames, townId } from './constants';
import { TownView, JoinTownRequest } from './messages';
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
export class EntryTownViewDelivery {
  handle(
    _entry: Entry,
    actor: Player,
    context: ZLinkMessageContext,
    message: TownView
  ): Promise<void> {
    return actor.deliver(message.townId, context.packetName, message);
  }
}
