import { Inject, Injectable } from '@nestjs/common';
import {
  ZLINK_SPOT_MANAGER,
  zlinkEntrySpotActorSendHandler,
  zlinkSpotActorSendHandler
} from '@zlink-systems/nestjs';
import {
  ZLinkSpotCreateState,
  type Type,
  type ZLinkMessageContext,
  type ZLinkSpotManager
} from '@zlink-systems/framework';
import { Names, PacketNames, Reasons } from '../../shared/constants';
import {
  ActionRejected,
  BetRequest,
  HandDealt,
  HandResult,
  JoinTableRequest,
  StartHandRequest,
  TableAdmission,
  TableView,
  TableViewRequest
} from '../../shared/messages';
import { Player } from './player';
import { Entry, Table } from './table';
@Injectable()
@zlinkEntrySpotActorSendHandler({
  entrySpot: () => Entry,
  actor: () => Player,
  packetName: PacketNames.joinTableRequest
})
export class EntryJoin {
  constructor(@Inject(ZLINK_SPOT_MANAGER) private readonly spots: ZLinkSpotManager) {}
  async handle(
    _spot: Entry,
    actor: Player,
    _context: ZLinkMessageContext,
    request: JoinTableRequest
  ): Promise<void> {
    if (actor.context.actorId !== request.playerId) throw new Error('Player identity mismatch');
    if (request.reconnect) {
      await actor.reject(Reasons.seated);
      return;
    }
    if (actor.match) {
      await actor.reject(Reasons.matchPending);
      return;
    }
    if (request.tableId.startsWith(Names.matchRoomPrefix)) {
      await actor.reject(Reasons.full);
      return;
    }
    const created = await this.spots
      .getOrCreate(request.tableId, Names.table)
      .inMesh(Names.mesh)
      .submit();
    if (created.state === ZLinkSpotCreateState.Rejected) throw new Error('Table creation rejected');
    const chips = await actor.loadPlayer();
    actor.context.joinSpot(request.tableId, new TableAdmission(request, chips)).defer();
  }
}
@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Table,
  actor: () => Player,
  packetName: PacketNames.joinTableRequest
})
export class Rejoin {
  async handle(
    spot: Table,
    actor: Player,
    _context: ZLinkMessageContext,
    request: JoinTableRequest
  ): Promise<void> {
    if (
      request.playerId !== actor.context.actorId ||
      request.tableId !== String(spot.context.spotId)
    )
      throw new Error('Reconnect identity mismatch');
    await spot.respond(actor);
  }
}
@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Table,
  actor: () => Player,
  packetName: PacketNames.tableViewRequest
})
export class View {
  async handle(
    spot: Table,
    actor: Player,
    _context: ZLinkMessageContext,
    _request: TableViewRequest
  ): Promise<void> {
    await spot.respond(actor);
  }
}
@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Table,
  actor: () => Player,
  packetName: PacketNames.startHandRequest
})
export class Start {
  async handle(
    spot: Table,
    actor: Player,
    _context: ZLinkMessageContext,
    _request: StartHandRequest
  ): Promise<void> {
    const reason = spot.canPlay ? spot.state.start(actor.context.actorId) : Reasons.matchPending;
    if (reason) await actor.context.boundSession.send(new ActionRejected(reason, 0)).submit();
    else await spot.deal();
  }
}
@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Table,
  actor: () => Player,
  packetName: PacketNames.betRequest
})
export class Bet {
  async handle(
    spot: Table,
    actor: Player,
    _context: ZLinkMessageContext,
    request: BetRequest
  ): Promise<void> {
    const result = spot.canPlay
      ? spot.state.bet(actor.context.actorId, request.handNumber, request.amount)
      : Reasons.matchPending;
    if (typeof result === 'string')
      await actor.context.boundSession.send(new ActionRejected(result, 0)).submit();
    else await spot.publish(result ?? null);
  }
}
const deliveryPackets: ReadonlyMap<string, Type<TableView | HandDealt | HandResult>> = new Map<
  string,
  Type<TableView | HandDealt | HandResult>
>([
  [PacketNames.tableView, TableView],
  [PacketNames.handDealt, HandDealt],
  [PacketNames.handResult, HandResult]
]);
@Injectable()
export class Deliver {
  async handle(
    _spot: Table,
    actor: Player,
    context: ZLinkMessageContext,
    message: TableView | HandDealt | HandResult
  ): Promise<void> {
    const type = deliveryPackets.get(context.packetName);
    if (!type) throw new Error(`Unregistered table push: ${context.packetName}`);
    if (context.packetName === PacketNames.handResult) {
      const seat = (message as HandResult).seats.find(
        (seat) => seat.playerId === actor.context.actorId
      );
      if (!seat) throw new Error('Player is missing from round settlement');
      actor.state.chips = seat.chips;
    }
    // Preserve the registered public DTO type when forwarding the received fields.
    await actor.context.boundSession.send(Object.assign(new type(), message)).submit();
  }
}
for (const packetName of deliveryPackets.keys())
  zlinkSpotActorSendHandler({ spot: () => Table, actor: () => Player, packetName })(Deliver);
