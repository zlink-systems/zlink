import { BorderVisible, FieldView } from '../shared/messages';
import { Injectable } from '@nestjs/common';
import { zlinkSpotActorSendHandler, zlinkSpotPacketHandler } from '@zlink-systems/nestjs';
import type { ZLinkMessageContext } from '@zlink-systems/framework';
import { PacketNames } from '../shared/constants';
import {
  JoinWorldRequest,
  ViewRequest,
  AdvanceRequest,
  DungeonView,
  PrepareDungeon,
  DungeonPrepared
} from '../shared/messages';
import { Player } from '../shared/player';
import { Dungeon } from './spots';
@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Dungeon,
  actor: () => Player,
  packetName: PacketNames.joinWorldRequest
})
export class Reconnect {
  async handle(
    spot: Dungeon,
    actor: Player,
    _context: ZLinkMessageContext,
    request: JoinWorldRequest
  ): Promise<void> {
    if (request.playerId !== actor.context.actorId) throw new Error('Player identity mismatch');
    await spot.show(actor);
  }
}
@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Dungeon,
  actor: () => Player,
  packetName: PacketNames.viewRequest
})
export class View {
  async handle(
    spot: Dungeon,
    actor: Player,
    _context: ZLinkMessageContext,
    _request: ViewRequest
  ): Promise<void> {
    await spot.show(actor);
  }
}
@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Dungeon,
  actor: () => Player,
  packetName: PacketNames.advanceRequest
})
export class Advance {
  async handle(
    spot: Dungeon,
    actor: Player,
    _context: ZLinkMessageContext,
    _request: AdvanceRequest
  ): Promise<void> {
    await spot.advance(actor);
  }
}
@Injectable()
@zlinkSpotPacketHandler({ spot: () => Dungeon, packetName: PacketNames.prepareDungeon })
export class Prepare {
  async handle(
    spot: Dungeon,
    request: PrepareDungeon,
    _context: ZLinkMessageContext
  ): Promise<DungeonPrepared> {
    return spot.prepare(request);
  }
}
@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Dungeon,
  actor: () => Player,
  packetName: PacketNames.dungeonView
})
export class Delivery {
  handle(
    _spot: Dungeon,
    actor: Player,
    context: ZLinkMessageContext,
    message: DungeonView
  ): Promise<void> {
    return actor.deliver(message.dungeonId, context.packetName, message, true);
  }
}

@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Dungeon,
  actor: () => Player,
  packetName: PacketNames.borderVisible
})
export class DepartedBorderVisible {
  handle(
    _spot: Dungeon,
    actor: Player,
    context: ZLinkMessageContext,
    message: BorderVisible
  ): Promise<void> {
    return actor.deliver(message.observerFieldId, context.packetName, message, true);
  }
}

@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Dungeon,
  actor: () => Player,
  packetName: PacketNames.fieldView
})
export class DepartedFieldView {
  handle(
    _spot: Dungeon,
    actor: Player,
    context: ZLinkMessageContext,
    message: FieldView
  ): Promise<void> {
    return actor.deliver(message.fieldId, context.packetName, message, true);
  }
}
