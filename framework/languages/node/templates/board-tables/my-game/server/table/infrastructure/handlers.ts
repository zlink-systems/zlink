import { Inject, Injectable } from '@nestjs/common';
import {
  ZLINK_SPOT_MANAGER,
  zlinkEntrySpotActorSendHandler,
  zlinkSpotActorSendHandler,
} from '@zlink-systems/nestjs';
import {
  ZLinkSpotCreateState,
  type ZLinkMessageContext,
  type ZLinkSpotManager,
} from '@zlink-systems/framework';
import { Names, PacketNames } from '../../shared/constants';
import {
  ActionRejected,
  BetRequest,
  DeliverTableState,
  HandDealt,
  HandResult,
  JoinTableRequest,
  StartHandRequest,
  TableView,
  TableViewRequest,
} from '../../shared/messages';
import { Player } from './player';
import { Entry, Table } from './table';
@Injectable()
@zlinkEntrySpotActorSendHandler({
  entrySpot: () => Entry,
  actor: () => Player,
  packetName: PacketNames.joinTableRequest,
})
export class EntryJoin {
  constructor(@Inject(ZLINK_SPOT_MANAGER) private readonly spots: ZLinkSpotManager) {}
  async handle(
    _spot: Entry,
    actor: Player,
    _context: ZLinkMessageContext,
    request: JoinTableRequest,
  ): Promise<void> {
    if (actor.context.actorId !== request.playerId) throw new Error('Player identity mismatch');
    const created = await this.spots
      .getOrCreate(request.tableId, Names.table)
      .inMesh(Names.mesh)
      .submit();
    if (created.state === ZLinkSpotCreateState.Rejected) throw new Error('Table creation rejected');
    actor.context.joinSpot(request.tableId, request).defer();
  }
}
@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Table,
  actor: () => Player,
  packetName: PacketNames.joinTableRequest,
})
export class Rejoin {
  async handle(
    spot: Table,
    actor: Player,
    _context: ZLinkMessageContext,
    request: JoinTableRequest,
  ): Promise<void> {
    if (
      request.playerId !== actor.context.actorId ||
      request.tableId !== String(spot.context.spotId)
    )
      throw new Error('Reconnect identity mismatch');
    await spot.sendState(actor.context.actorId);
  }
}
@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Table,
  actor: () => Player,
  packetName: PacketNames.tableViewRequest,
})
export class View {
  async handle(
    spot: Table,
    actor: Player,
    _context: ZLinkMessageContext,
    _request: TableViewRequest,
  ): Promise<void> {
    await spot.sendState(actor.context.actorId);
  }
}
@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Table,
  actor: () => Player,
  packetName: PacketNames.startHandRequest,
})
export class Start {
  async handle(
    spot: Table,
    actor: Player,
    _context: ZLinkMessageContext,
    _request: StartHandRequest,
  ): Promise<void> {
    const reason = spot.state.start(actor.context.actorId);
    if (reason) await actor.context.boundSession.send(new ActionRejected(reason)).submit();
    else await spot.publish();
  }
}
@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Table,
  actor: () => Player,
  packetName: PacketNames.betRequest,
})
export class Bet {
  async handle(
    spot: Table,
    actor: Player,
    _context: ZLinkMessageContext,
    request: BetRequest,
  ): Promise<void> {
    const result = spot.state.bet(actor.context.actorId, request.handNumber, request.amount);
    if (typeof result === 'string')
      await actor.context.boundSession.send(new ActionRejected(result)).submit();
    else await spot.publish(result ?? null);
  }
}
@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => Table,
  actor: () => Player,
  packetName: PacketNames.deliverTableState,
})
export class Deliver {
  async handle(
    _spot: Table,
    actor: Player,
    _context: ZLinkMessageContext,
    message: DeliverTableState,
  ): Promise<void> {
    const v = message.view;
    await actor.context.boundSession
      .send(
        new TableView(
          v.tableId,
          v.nodeId,
          actor.context.actorId,
          actor.context.objectGeneration,
          v.yourSeat,
          v.handNumber,
          v.inHand,
          v.turnSeat,
          v.pot,
          v.seats,
        ),
      )
      .submit();
    if (message.cards.length)
      await actor.context.boundSession.send(new HandDealt(v.handNumber, message.cards)).submit();
    if (message.result) {
      const r = message.result;
      await actor.context.boundSession
        .send(new HandResult(r.handNumber, r.pot, r.winners, r.hands, r.seats))
        .submit();
    }
  }
}
