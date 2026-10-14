import { Inject, Injectable } from "@nestjs/common";
import {
  ZLINK_SPOT_MANAGER,
  zlinkEntrySpotActorSendHandler,
  zlinkSpotActorSendHandler,
  zlinkSpotTimerHandler,
} from "@zlink-systems/nestjs";
import {
  ZLinkSpotCreateState,
  type ZLinkMessageContext,
  type ZLinkSpotManager,
  type ZLinkPublishMessageContext,
  type ZLinkTimerTick,
} from "@zlink-systems/framework";
import { Names, PacketNames, zoneId } from "../shared/constants";
import {
  JoinWorldRequest,
  MoveRequest,
  EnterZone,
  BorderPresence,
  BorderVisible,
  OpenDungeonRequest,
  DungeonOpened,
} from "../shared/messages";
import { Entry, Zone } from "./zone";
import { Player } from "./player";
@Injectable()
@zlinkEntrySpotActorSendHandler({
  entrySpot: () => Entry,
  actor: () => Player,
  packetName: PacketNames.joinWorldRequest,
})
export class EnterWorld {
  async handle(
    _spot: Entry,
    actor: Player,
    _context: ZLinkMessageContext,
    request: JoinWorldRequest,
  ): Promise<void> {
    if (request.playerId !== actor.context.actorId)
      throw new Error("Player identity mismatch");
    actor.move(new EnterZone(request.x, 0));
  }
}
@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Zone,
  actor: () => Player,
  packetName: PacketNames.joinWorldRequest,
})
export class Reconnect {
  async handle(
    spot: Zone,
    actor: Player,
    _context: ZLinkMessageContext,
    request: JoinWorldRequest,
  ): Promise<void> {
    if (request.playerId !== actor.context.actorId)
      throw new Error("Player identity mismatch");
    spot.remember(actor);
    await actor.sendState();
  }
}
@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Zone,
  actor: () => Player,
  packetName: PacketNames.moveRequest,
})
export class Move {
  async handle(
    spot: Zone,
    actor: Player,
    _context: ZLinkMessageContext,
    request: MoveRequest,
  ): Promise<void> {
    if (request.sequence !== actor.sequence + 1)
      throw new Error("Move sequence must increase by one");
    const position = new EnterZone(request.x, request.sequence);
    if (zoneId(request.x) !== String(spot.context.spotId)) {
      actor.move(position);
      return;
    }
    actor.apply(position);
    spot.remember(actor);
    await actor.sendState();
  }
}
@Injectable()
@zlinkSpotTimerHandler({ spot: () => Zone })
export class BorderTick {
  async handle(spot: Zone, _tick: ZLinkTimerTick): Promise<void> {
    await spot.publishBorder();
  }
}
@Injectable()
export class BorderHandler {
  async handle(
    spot: Zone,
    presence: BorderPresence,
    _context: ZLinkPublishMessageContext,
  ): Promise<void> {
    await spot.showBorder(presence);
  }
}
@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Zone,
  actor: () => Player,
  packetName: PacketNames.borderVisible,
})
export class BorderDelivery {
  async handle(
    _spot: Zone,
    actor: Player,
    context: ZLinkMessageContext,
    presence: BorderVisible,
  ): Promise<void> {
    const type = deliveryPackets.get(context.packetName);
    if (!type) throw new Error(`Unregistered zone push: ${context.packetName}`);
    // Restore the declared public DTO class before forwarding (#1731).
    await actor.context.boundSession
      .send(Object.assign(new type(), presence))
      .submit();
  }
}
const deliveryPackets: ReadonlyMap<string, typeof BorderVisible> = new Map([
  [PacketNames.borderVisible, BorderVisible],
]);
@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Zone,
  actor: () => Player,
  packetName: PacketNames.openDungeonRequest,
})
export class OpenDungeon {
  constructor(
    @Inject(ZLINK_SPOT_MANAGER) private readonly spots: ZLinkSpotManager,
  ) {}
  async handle(
    spot: Zone,
    actor: Player,
    _context: ZLinkMessageContext,
    request: OpenDungeonRequest,
  ): Promise<void> {
    const result = await this.spots
      .getOrCreate(request.dungeonId, Names.dungeon)
      .inMesh(Names.mesh)
      .submit();
    if (result.state === ZLinkSpotCreateState.Rejected)
      throw new Error("Dungeon creation rejected");
    const opened = await spot.context.outbound
      .requestToSpot(
        request.dungeonId,
        new OpenDungeonRequest(request.dungeonId),
      )
      .submit<DungeonOpened>();
    await actor.context.boundSession
      .send(new DungeonOpened(opened.dungeonId, opened.nodeId))
      .submit();
  }
}
