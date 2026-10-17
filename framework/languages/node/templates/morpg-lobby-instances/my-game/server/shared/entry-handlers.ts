import { Injectable } from '@nestjs/common';
import { zlinkEntrySpotActorSendHandler } from '@zlink-systems/nestjs';
import type { ZLinkMessageContext } from '@zlink-systems/framework';
import { Names, PacketNames } from './constants';
import {
  PartyView,
  LobbyView,
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
    request: CreatePartyRequest
  ): Promise<void> {
    await spot.join(actor, request.partyId, true);
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
export class EntryPartyViewDelivery {
  handle(
    _entry: Entry,
    actor: Player,
    context: ZLinkMessageContext,
    message: PartyView
  ): Promise<void> {
    return actor.deliver(Names.partyPrefix + message.partyId, context.packetName, message);
  }
}
