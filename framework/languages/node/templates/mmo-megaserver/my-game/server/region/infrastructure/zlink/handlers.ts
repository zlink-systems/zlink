import { Injectable } from '@nestjs/common';
import { zlinkEntrySpotActorSendHandler, zlinkSpotActorSendHandler } from '@zlink-systems/nestjs';
import type { ZLinkMessageContext } from '@zlink-systems/framework';
import { Game, PacketNames, Rejection } from '../../../shared/constants';
import {
  EnterRegionRequest,
  RegionFull,
  RegionView,
  MoveRequest,
  ViewRequest,
  OpenDungeonRequest
} from '../../../shared/messages';
import { Entry, Region } from './spots';
import { Player } from './player';
@Injectable()
@zlinkEntrySpotActorSendHandler({
  entrySpot: () => Entry,
  actor: () => Player,
  packetName: PacketNames.enterRegionRequest
})
export class EnterRegion {
  async handle(
    entry: Entry,
    actor: Player,
    _context: ZLinkMessageContext,
    request: EnterRegionRequest
  ): Promise<void> {
    if (request.playerId !== actor.context.actorId) throw new Error('Player identity mismatch');
    await entry.select(actor, request.region, Game.firstIndex);
  }
}
@Injectable()
@zlinkEntrySpotActorSendHandler({
  entrySpot: () => Entry,
  actor: () => Player,
  packetName: PacketNames.regionFull
})
export class NextRegion {
  async handle(
    entry: Entry,
    actor: Player,
    _context: ZLinkMessageContext,
    request: RegionFull
  ): Promise<void> {
    if (request.reason !== Rejection.regionFull) throw new Error('Unknown region rejection');
    await entry.select(actor, request.region, request.index + 1);
  }
}
@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Region,
  actor: () => Player,
  packetName: PacketNames.enterRegionRequest
})
export class ReconnectRegion {
  async handle(
    spot: Region,
    actor: Player,
    _context: ZLinkMessageContext,
    request: EnterRegionRequest
  ): Promise<void> {
    if (request.playerId !== actor.context.actorId) throw new Error('Player identity mismatch');
    await spot.show(actor);
  }
}
@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Region,
  actor: () => Player,
  packetName: PacketNames.moveRequest
})
export class Move {
  async handle(
    spot: Region,
    actor: Player,
    _context: ZLinkMessageContext,
    request: MoveRequest
  ): Promise<void> {
    await spot.move(actor, request);
  }
}
@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Region,
  actor: () => Player,
  packetName: PacketNames.viewRequest
})
export class View {
  async handle(
    spot: Region,
    actor: Player,
    _context: ZLinkMessageContext,
    _request: ViewRequest
  ): Promise<void> {
    await spot.show(actor);
  }
}
@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Region,
  actor: () => Player,
  packetName: PacketNames.openDungeonRequest
})
export class OpenDungeon {
  async handle(
    spot: Region,
    actor: Player,
    _context: ZLinkMessageContext,
    request: OpenDungeonRequest
  ): Promise<void> {
    await spot.open(actor, request.dungeonId);
  }
}
const deliveryPackets: ReadonlyMap<string, new () => object> = new Map([
  [PacketNames.regionView, RegionView]
]);
@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Region,
  actor: () => Player,
  packetName: PacketNames.regionView
})
export class Delivery {
  async handle(
    _spot: Region,
    actor: Player,
    context: ZLinkMessageContext,
    message: RegionView
  ): Promise<void> {
    const type = deliveryPackets.get(context.packetName);
    if (!type) throw new Error(`Unregistered region push: ${context.packetName}`);
    // Restore the declared public DTO class before forwarding (#1731).
    await actor.context.boundSession.send(Object.assign(new type(), message)).submit();
  }
}
