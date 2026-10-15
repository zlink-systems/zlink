import { Inject, Injectable, Scope } from '@nestjs/common';
import { ZLINK_ACTOR_CLIENT } from '@zlink-systems/nestjs';
import type {
  ZLinkActorClient,
  ZLinkEntrySpot,
  ZLinkEntrySpotContext,
  ZLinkMessage,
  ZLinkSpot,
  ZLinkSpotContext,
  ZLinkSpotRelocationAdapter
} from '@zlink-systems/framework';
import { NODE_CONFIG, type NodeConfig } from '../../shared/config';
import { TableView, HandDealt, JoinTableRequest, type HandResult } from '../../shared/messages';
import { TableState, type TableSnapshot } from '../domain/table-state';
import { Player } from './player';
@Injectable({ scope: Scope.TRANSIENT })
export class Entry implements ZLinkEntrySpot<Player> {
  readonly context!: ZLinkEntrySpotContext<Player, Entry>;
  async onJoinedActor(_actor: Player): Promise<void> {}
  async onLeaveActor(_actor: Player): Promise<void> {}
  async onDisconnectActor(_actor: Player): Promise<void> {}
}
@Injectable({ scope: Scope.TRANSIENT })
export class Table implements ZLinkSpot<Player> {
  readonly context!: ZLinkSpotContext<Player, Table>;
  readonly state = new TableState();
  constructor(
    @Inject(NODE_CONFIG) private readonly config: NodeConfig,
    @Inject(ZLINK_ACTOR_CLIENT) private readonly actors: ZLinkActorClient
  ) {}
  async onActorJoin(actorId: string, request: ZLinkMessage) {
    const join = request.decode(JoinTableRequest);
    if (join.playerId !== actorId || join.tableId !== String(this.context.spotId))
      throw new Error('Join identity mismatch');
    return this.state.admission(actorId) ? { accepted: false } : { accepted: true };
  }
  async onJoinedActor(actor: Player): Promise<void> {
    this.state.join(actor.context.actorId);
    await this.publish();
  }
  async onLeaveActor(_actor: Player): Promise<void> {}
  async onDisconnectActor(_actor: Player): Promise<void> {}
  view(playerId: string): TableView {
    const v = this.state.view(playerId, String(this.context.spotId), this.config.id);
    return new TableView(
      v.tableId,
      v.nodeId,
      v.yourSeat,
      v.handNumber,
      v.inHand,
      v.turnSeat,
      v.pot,
      v.seats
    );
  }
  async respond(actor: Player): Promise<void> {
    const playerId = actor.context.actorId;
    await actor.context.boundSession.send(this.view(playerId)).submit();
    const cards = this.state.cards(playerId);
    if (cards.length)
      await actor.context.boundSession
        .send(new HandDealt(this.state.snapshot.handNumber, cards))
        .submit();
  }
  async publish(result: HandResult | null = null): Promise<void> {
    for (const seat of this.state.snapshot.seats) {
      await this.actors.sendToActor(seat.playerId, this.view(seat.playerId)).submit();
      if (result) await this.actors.sendToActor(seat.playerId, result).submit();
    }
  }
  async deal(): Promise<void> {
    await this.publish();
    for (const seat of this.state.snapshot.seats)
      await this.actors
        .sendToActor(
          seat.playerId,
          new HandDealt(this.state.snapshot.handNumber, this.state.cards(seat.playerId))
        )
        .submit();
  }
}
@Injectable()
export class TableRelocation implements ZLinkSpotRelocationAdapter<Table> {
  async capture(spot: Table): Promise<Uint8Array> {
    return Buffer.from(JSON.stringify(spot.state.snapshot));
  }
  async restore(spot: Table, payload: Uint8Array): Promise<void> {
    spot.state.snapshot = JSON.parse(Buffer.from(payload).toString('utf8')) as TableSnapshot;
  }
}
