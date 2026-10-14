import { Inject, Injectable, Scope } from '@nestjs/common';
import { ZLINK_ACTOR_CLIENT } from '@zlink-systems/nestjs';
import type {
  ZLinkActorClient,
  ZLinkEntrySpot,
  ZLinkEntrySpotContext,
  ZLinkMessage,
  ZLinkSpot,
  ZLinkSpotContext,
  ZLinkSpotRelocationAdapter,
} from '@zlink-systems/framework';
import { NODE_CONFIG, type NodeConfig } from '../../shared/config';
import {
  ActionRejected,
  DeliverTableState,
  JoinTableRequest,
  type HandResult,
} from '../../shared/messages';
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
    @Inject(ZLINK_ACTOR_CLIENT) private readonly actors: ZLinkActorClient,
  ) {}
  async onActorJoin(actorId: string, request: ZLinkMessage) {
    const join = request.decode(JoinTableRequest);
    if (join.playerId !== actorId || join.tableId !== String(this.context.spotId))
      throw new Error('Join identity mismatch');
    const reason = this.state.admission(actorId);
    return reason ? { accepted: false, reply: new ActionRejected(reason) } : { accepted: true };
  }
  async onJoinedActor(actor: Player): Promise<void> {
    this.state.join(actor.context.actorId);
    await this.publish();
  }
  async onLeaveActor(_actor: Player): Promise<void> {}
  async onDisconnectActor(_actor: Player): Promise<void> {}
  async sendState(playerId: string, result: HandResult | null = null): Promise<void> {
    await this.actors
      .sendToActor(
        playerId,
        new DeliverTableState(
          this.state.view(playerId, String(this.context.spotId), this.config.id),
          this.state.cards(playerId),
          result,
        ),
      )
      .submit();
  }
  async publish(result: HandResult | null = null): Promise<void> {
    for (const seat of this.state.snapshot.seats) await this.sendState(seat.playerId, result);
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
