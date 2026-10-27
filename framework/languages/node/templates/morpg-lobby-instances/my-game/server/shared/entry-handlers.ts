import { Injectable } from '@nestjs/common';
import { zlinkEntrySpotActorSendHandler } from '@zlink-systems/nestjs';
import type { ZLinkMessageContext } from '@zlink-systems/framework';
import { PacketNames } from './constants';
import {
  PartyView,
  DungeonView,
  ActionRejected,
  LobbyView,
  newPartyId,
  type JoinLobbyRequest,
  type CreatePartyRequest,
  type JoinPartyRequest
} from './messages';
import { Player } from './player';
import { Entry } from './entry';
@Injectable()
@zlinkEntrySpotActorSendHandler({
  entrySpot: () => Entry,
  actor: () => Player,
  packetName: PacketNames.joinLobbyRequest
})
export class Login {
  async handle(
    _spot: Entry,
    actor: Player,
    _context: ZLinkMessageContext,
    request: JoinLobbyRequest
  ): Promise<void> {
    if (request.playerId !== actor.context.actorId) throw new Error('Player identity mismatch');
    await actor.context.boundSession.send(new LobbyView()).submit();
  }
}
@Injectable()
@zlinkEntrySpotActorSendHandler({
  entrySpot: () => Entry,
  actor: () => Player,
  packetName: PacketNames.createPartyRequest
})
export class CreateParty {
  async handle(
    spot: Entry,
    actor: Player,
    _context: ZLinkMessageContext,
    _request: CreatePartyRequest
  ): Promise<void> {
    await spot.join(actor, newPartyId(), true);
  }
}
@Injectable()
@zlinkEntrySpotActorSendHandler({
  entrySpot: () => Entry,
  actor: () => Player,
  packetName: PacketNames.joinPartyRequest
})
export class JoinParty {
  async handle(
    spot: Entry,
    actor: Player,
    _context: ZLinkMessageContext,
    request: JoinPartyRequest
  ): Promise<void> {
    await spot.join(actor, request.partyId, false);
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
    message: PartyView | DungeonView | ActionRejected
  ): Promise<void> {
    await actor.deliver(context.packetName, message);
  }
}
