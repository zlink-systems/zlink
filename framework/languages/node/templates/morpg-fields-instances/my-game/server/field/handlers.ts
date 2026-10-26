import { Injectable } from '@nestjs/common';
import { zlinkSpotActorSendHandler } from '@zlink-systems/nestjs';
import type {
  ZLinkMessageContext,
  ZLinkPublishMessageContext,
  ZLinkTimerTick
} from '@zlink-systems/framework';
import { PacketNames } from '../shared/constants';
import {
  DungeonView,
  JoinWorldRequest,
  MoveRequest,
  BorderPresence,
  BorderVisible,
  CreatePartyRequest,
  JoinPartyRequest,
  EnterDungeonRequest,
  JoinDungeon,
  PartyView,
  ViewRequest
} from '../shared/messages';
import { Player } from '../shared/player';
import { Field } from './spots';
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
    await spot.move(actor, request);
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
    _request: CreatePartyRequest
  ): Promise<void> {
    await spot.party(actor, '', true);
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
      .joinSpot(request.dungeonId, new JoinDungeon(request.dungeonId, request.runId))
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
@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Field,
  actor: () => Player,
  packetName: PacketNames.partyView
})
export class PartyDelivery {
  async handle(
    _spot: Field,
    actor: Player,
    context: ZLinkMessageContext,
    message: PartyView
  ): Promise<void> {
    await actor.deliver(context.packetName, message);
  }
}

@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Field,
  actor: () => Player,
  packetName: PacketNames.borderVisible
})
export class Delivery {
  async handle(
    spot: Field,
    actor: Player,
    context: ZLinkMessageContext,
    message: BorderVisible
  ): Promise<void> {
    await actor.deliver(context.packetName, message, spot.isVisible(actor));
  }
}

@Injectable()
@zlinkSpotActorSendHandler({ spot: () => Field, actor: () => Player, packetName: PacketNames.viewRequest })
export class FieldQuery {
  async handle(spot: Field, actor: Player, _context: ZLinkMessageContext, _request: ViewRequest): Promise<void> {
    await spot.show(actor);
  }
}

@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Field,
  actor: () => Player,
  packetName: PacketNames.dungeonView
})
export class FieldNotifications {
  async handle(
    spot: Field,
    actor: Player,
    context: ZLinkMessageContext,
    message: DungeonView
  ): Promise<void> {
    await actor.deliver(context.packetName, message, spot.isVisible(actor));
  }
}
