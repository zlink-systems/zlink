import { Inject, Injectable } from '@nestjs/common';
import {
  ZLINK_SPOT_MANAGER,
  zlinkEntrySpotActorSendHandler,
  zlinkSpotActorSendHandler
} from '@zlink-systems/nestjs';
import {
  ZLinkSpotCreateState,
  type ZLinkMessageContext,
  type ZLinkSpotManager,
  type ZLinkPublishMessageContext
} from '@zlink-systems/framework';
import { Names, PacketNames } from '../shared/constants';
import {
  ActionRejected,
  JoinWorldRequest,
  MoveRequest,
  EnterZone,
  BorderPresence,
  OpenDungeonRequest,
  DungeonOpened,
  PlayerAppeared,
  PlayerLeft,
  PlayerMoved,
  Position,
  WorldState
} from '../shared/messages';
import { Entry, Zone } from './zone';
import { Player } from './player';
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
    actor.move(new EnterZone(new Position(request.x), 0n, 0));
  }
}
@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Zone,
  actor: () => Player,
  packetName: PacketNames.joinWorldRequest
})
export class Reconnect {
  async handle(
    spot: Zone,
    actor: Player,
    _context: ZLinkMessageContext,
    request: JoinWorldRequest
  ): Promise<void> {
    if (request.playerId !== actor.context.actorId) throw new Error('Player identity mismatch');
    const previous = spot.state(actor.context.actorId);
    if (!previous) {
      await actor.context.boundSession.send(new ActionRejected('outside-world', 0)).submit();
      return;
    }
    spot.setConnected(actor.context.actorId, true);
    if (!previous.connected)
      await spot.publishLocal(actor.context.actorId, new PlayerAppeared(actor.context.actorId, previous.position));
    await actor.context.boundSession
      .send(new WorldState(String(spot.context.spotId), spot.nodeId, previous.position.x, previous.sequence))
      .submit();
  }
}
@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Zone,
  actor: () => Player,
  packetName: PacketNames.moveRequest
})
export class Move {
  async handle(
    spot: Zone,
    actor: Player,
    _context: ZLinkMessageContext,
    request: MoveRequest
  ): Promise<void> {
    const movement = spot.move(actor.context.actorId, request);
    if (movement.rejection) {
      await actor.context.boundSession.send(movement.rejection).submit();
      return;
    }
    const destination = new EnterZone(movement.position, movement.sequence, movement.remainingMs);
    if (movement.relocates) {
      await spot.leaveForZone(actor.context.actorId);
      actor.move(destination);
      return;
    }
    await spot.publishLocal(actor.context.actorId, new PlayerMoved(actor.context.actorId, movement.position, movement.sequence));
    await spot.publishBorder(actor.context.actorId, movement.position, movement.sequence, 'moved', null);
    await actor.context.boundSession
      .send(new WorldState(String(spot.context.spotId), spot.nodeId, movement.position.x, movement.sequence))
      .submit();
  }
}
@Injectable()
export class BorderHandler {
  async handle(
    spot: Zone,
    presence: BorderPresence,
    _context: ZLinkPublishMessageContext
  ): Promise<void> {
    await spot.showBorder(presence);
  }
}
@Injectable()
@zlinkSpotActorSendHandler({ spot: () => Zone, actor: () => Player, packetName: PacketNames.playerAppeared })
export class PlayerAppearedDelivery {
  async handle(_spot: Zone, actor: Player, _context: ZLinkMessageContext, event: PlayerAppeared): Promise<void> {
    await actor.context.boundSession.send(new PlayerAppeared(event.playerId, event.position)).submit();
  }
}
@Injectable()
@zlinkSpotActorSendHandler({ spot: () => Zone, actor: () => Player, packetName: PacketNames.playerMoved })
export class PlayerMovedDelivery {
  async handle(_spot: Zone, actor: Player, _context: ZLinkMessageContext, event: PlayerMoved): Promise<void> {
    await actor.context.boundSession.send(new PlayerMoved(event.playerId, event.position, event.sequence)).submit();
  }
}
@Injectable()
@zlinkSpotActorSendHandler({ spot: () => Zone, actor: () => Player, packetName: PacketNames.playerLeft })
export class PlayerLeftDelivery {
  async handle(_spot: Zone, actor: Player, _context: ZLinkMessageContext, event: PlayerLeft): Promise<void> {
    await actor.context.boundSession.send(new PlayerLeft(event.playerId, event.reason)).submit();
  }
}
@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Zone,
  actor: () => Player,
  packetName: PacketNames.openDungeonRequest
})
export class OpenDungeon {
  constructor(@Inject(ZLINK_SPOT_MANAGER) private readonly spots: ZLinkSpotManager) {}
  async handle(
    spot: Zone,
    actor: Player,
    _context: ZLinkMessageContext,
    request: OpenDungeonRequest
  ): Promise<void> {
    const result = await this.spots
      .getOrCreate(request.dungeonId, Names.dungeon)
      .inMesh(Names.mesh)
      .submit();
    if (result.state === ZLinkSpotCreateState.Rejected)
      throw new Error('Dungeon creation rejected');
    const opened = await spot.context.outbound
      .requestToSpot(request.dungeonId, new OpenDungeonRequest(request.dungeonId))
      .submit<DungeonOpened>();
    await actor.context.boundSession
      .send(new DungeonOpened(opened.dungeonId, opened.nodeId))
      .submit();
  }
}
