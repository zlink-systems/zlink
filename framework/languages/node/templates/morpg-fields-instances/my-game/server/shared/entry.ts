import { Injectable, Scope } from '@nestjs/common';
import { zlinkEntrySpotActorSendHandler } from '@zlink-systems/nestjs';
import type {
  ZLinkEntrySpot,
  ZLinkEntrySpotContext,
  ZLinkMessageContext
} from '@zlink-systems/framework';
import { PacketNames } from './constants';
import { PartyView, BorderVisible, DungeonView, JoinWorldRequest, EnterField } from './messages';
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
  packetName: PacketNames.joinWorldRequest
})
export class EnterWorld {
  async handle(
    _spot: Entry,
    actor: Player,
    _context: ZLinkMessageContext,
    request: JoinWorldRequest
  ): Promise<void> {
    if (request.playerId !== actor.context.actorId) throw new Error('Player identity mismatch');
    actor.move(new EnterField(request.x, 0n));
  }
}

@Injectable()
@zlinkEntrySpotActorSendHandler({
  entrySpot: () => Entry,
  actor: () => Player,
  packetName: PacketNames.partyView
})
@zlinkEntrySpotActorSendHandler({
  entrySpot: () => Entry,
  actor: () => Player,
  packetName: PacketNames.borderVisible
})
@zlinkEntrySpotActorSendHandler({
  entrySpot: () => Entry,
  actor: () => Player,
  packetName: PacketNames.dungeonView
})
export class EntryNotifications {
  async handle(
    _spot: Entry,
    actor: Player,
    context: ZLinkMessageContext,
    message: PartyView | BorderVisible | DungeonView
  ): Promise<void> {
    await actor.deliver(context.packetName, message);
  }
}
