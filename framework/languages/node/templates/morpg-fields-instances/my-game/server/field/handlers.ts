import { Injectable } from '@nestjs/common';
import { zlinkEntrySpotActorSendHandler, zlinkSpotActorSendHandler } from '@zlink-systems/nestjs';
import type {
  ZLinkMessageContext,
  ZLinkPublishMessageContext,
  ZLinkTimerTick
} from '@zlink-systems/framework';
import { PacketNames, fieldId } from '../shared/constants';
import {
  JoinWorldRequest,
  MoveRequest,
  EnterField,
  BorderPresence,
  BorderVisible,
  CreatePartyRequest,
  JoinPartyRequest,
  EnterDungeonRequest,
  JoinDungeon,
  FieldView
} from '../shared/messages';
import { Player } from '../shared/player';
import { Entry, Field } from './spots';
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
    actor.move(new EnterField(request.x, 0));
  }
}
@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Field,
  actor: () => Player,
  packetName: PacketNames.joinWorldRequest
})
export class Reconnect {
  async handle(
    spot: Field,
    actor: Player,
    _context: ZLinkMessageContext,
    request: JoinWorldRequest
  ): Promise<void> {
    if (request.playerId !== actor.context.actorId) throw new Error('Player identity mismatch');
    spot.remember(actor);
    await actor.sendState();
  }
}
@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Field,
  actor: () => Player,
  packetName: PacketNames.moveRequest
})
export class Move {
  async handle(
    spot: Field,
    actor: Player,
    _context: ZLinkMessageContext,
    request: MoveRequest
  ): Promise<void> {
    if (request.sequence !== actor.sequence + 1)
      throw new Error('Move sequence must increase by one');
    const position = new EnterField(request.x, request.sequence);
    if (fieldId(request.x) !== String(spot.context.spotId)) actor.move(position);
    else {
      actor.apply(position);
      spot.remember(actor);
      await actor.sendState();
    }
  }
}
@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Field,
  actor: () => Player,
  packetName: PacketNames.createPartyRequest
})
export class CreateParty {
  async handle(
    spot: Field,
    actor: Player,
    _context: ZLinkMessageContext,
    request: CreatePartyRequest
  ): Promise<void> {
    await spot.party(actor, request.partyId, true);
  }
}
@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Field,
  actor: () => Player,
  packetName: PacketNames.joinPartyRequest
})
export class JoinParty {
  async handle(
    spot: Field,
    actor: Player,
    _context: ZLinkMessageContext,
    request: JoinPartyRequest
  ): Promise<void> {
    await spot.party(actor, request.partyId, false);
  }
}
@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Field,
  actor: () => Player,
  packetName: PacketNames.enterDungeonRequest
})
export class EnterDungeon {
  async handle(
    spot: Field,
    actor: Player,
    _context: ZLinkMessageContext,
    _request: EnterDungeonRequest
  ): Promise<void> {
    await spot.enter(actor);
  }
}
@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Field,
  actor: () => Player,
  packetName: PacketNames.joinDungeon
})
export class DungeonTravel {
  async handle(
    _spot: Field,
    actor: Player,
    _context: ZLinkMessageContext,
    request: JoinDungeon
  ): Promise<void> {
    actor.context
      .joinSpot(request.dungeonId, new JoinDungeon(request.dungeonId, request.partyId))
      .defer();
  }
}
@Injectable()
export class BorderTick {
  async handle(spot: Field, _tick: ZLinkTimerTick): Promise<void> {
    await spot.publishBorder();
  }
}
@Injectable()
export class BorderHandler {
  async handle(
    spot: Field,
    message: BorderPresence,
    _context: ZLinkPublishMessageContext
  ): Promise<void> {
    await spot.showBorder(message);
  }
}
const deliveryPackets: ReadonlyMap<string, new () => object> = new Map<string, new () => object>([
  [PacketNames.fieldView, FieldView],
  [PacketNames.borderVisible, BorderVisible]
]);
@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Field,
  actor: () => Player,
  packetName: PacketNames.fieldView
})
@zlinkSpotActorSendHandler({
  spot: () => Field,
  actor: () => Player,
  packetName: PacketNames.borderVisible
})
export class Delivery {
  async handle(
    _spot: Field,
    actor: Player,
    context: ZLinkMessageContext,
    message: FieldView | BorderVisible
  ): Promise<void> {
    const type = deliveryPackets.get(context.packetName);
    if (!type) throw new Error(`Unregistered field push: ${context.packetName}`);
    // Restore the declared public DTO class before forwarding (#1731).
    await actor.context.boundSession.send(Object.assign(new type(), message)).submit();
  }
}
