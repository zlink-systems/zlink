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
import { Names, Reasons } from '../../shared/constants';
import { NODE_CONFIG, type NodeConfig } from '../../shared/config';
import {
  TableView,
  HandDealt,
  TableAdmission,
  MatchCommitted,
  RoomAdmission,
  RoomWithdrawn,
  type HandResult,
  type PrepareRoom
} from '../../shared/messages';
import { PlayerRecords } from '../application/player-records';
import { TableState } from '../domain/table-state';
import { Player } from './player';
import { deserializeTableSnapshot, serializeTableSnapshot } from './table-snapshot-json';
@Injectable({ scope: Scope.TRANSIENT })
export class Entry implements ZLinkEntrySpot<Player> {
  readonly context!: ZLinkEntrySpotContext<Player, Entry>;
  async onJoinedActor(_actor: Player): Promise<void> {}
  async onLeaveActor(_actor: Player): Promise<void> {}
  async onDisconnectActor(actor: Player): Promise<void> {
    await actor.disconnected(this.context.outbound);
  }
}
interface RoomAssignment {
  id: string;
  members: string[];
  phase: 'prepared' | 'committed' | 'withdrawn';
}

@Injectable({ scope: Scope.TRANSIENT })
export class Table implements ZLinkSpot<Player> {
  readonly context!: ZLinkSpotContext<Player, Table>;
  readonly state = new TableState();
  assignment?: RoomAssignment;

  get canPlay(): boolean {
    return !this.isMatchRoom || this.assignment?.phase === 'committed';
  }

  private get isMatchRoom(): boolean {
    return String(this.context.spotId).startsWith(Names.matchRoomPrefix);
  }
  constructor(
    @Inject(NODE_CONFIG) private readonly config: NodeConfig,
    @Inject(ZLINK_ACTOR_CLIENT) private readonly actors: ZLinkActorClient,
    private readonly records: PlayerRecords
  ) {}
  async onActorJoin(actorId: string, request: ZLinkMessage) {
    if (this.isMatchRoom) {
      const assignment = this.assignment;
      if (!assignment || assignment.phase !== 'prepared' || !assignment.members.includes(actorId))
        return { accepted: false };
      const admission = request.decode(RoomAdmission);
      return {
        accepted:
          admission.assignmentId === assignment.id && this.state.admit(actorId, admission.chips)
      };
    }
    const { join, chips } = request.decode(TableAdmission);
    if (join.playerId !== actorId || join.tableId !== String(this.context.spotId))
      throw new Error('Join identity mismatch');
    return { accepted: this.state.admit(actorId, chips) };
  }
  async onJoinedActor(_actor: Player): Promise<void> {
    if (!this.isMatchRoom) await this.publish();
  }
  prepare(request: PrepareRoom): boolean {
    if (
      String(this.context.spotId) !== request.assignmentId ||
      !this.isMatchRoom ||
      request.members.length !== 2 ||
      new Set(request.members).size !== request.members.length
    )
      return false;
    this.assignment ??= {
      id: request.assignmentId,
      members: request.members,
      phase: 'prepared'
    };
    return (
      this.assignment.phase === 'prepared' &&
      this.assignment.members.every((member, index) => member === request.members[index])
    );
  }
  async commit(assignmentId: string): Promise<void> {
    const assignment = this.assignment;
    if (!assignment || assignment.id !== assignmentId || assignment.phase !== 'prepared') return;
    const seats = this.state.snapshot.seats;
    const reason = this.state.start(seats[0].playerId);
    if (reason) throw new Error(`Cannot start matched hand: ${reason}`);
    assignment.phase = 'committed';
    for (const seat of seats)
      await this.actors
        .sendToActor(
          seat.playerId,
          new MatchCommitted(assignmentId, this.state.seatOf(seat.playerId))
        )
        .submit();
    await this.deal();
  }
  async withdraw(assignmentId: string): Promise<void> {
    const assignment = this.assignment;
    if (!assignment || assignment.id !== assignmentId || assignment.phase === 'committed') return;
    assignment.phase = 'withdrawn';
    await this.context.outbound.sendToSpot(Names.poolKey, new RoomWithdrawn(assignmentId)).submit();
  }
  async onLeaveActor(_actor: Player): Promise<void> {}
  async onDisconnectActor(actor: Player): Promise<void> {
    await actor.disconnected(this.context.outbound);
  }
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
    if (!this.canPlay) {
      await actor.reject(Reasons.matchPending);
      return;
    }
    const playerId = actor.context.actorId;
    await actor.context.boundSession.send(this.view(playerId)).submit();
    await actor.context.boundSession.send(this.handFor(playerId)).submit();
  }
  private handFor(playerId: string): HandDealt {
    return new HandDealt(this.state.snapshot.handNumber, this.state.cards(playerId));
  }
  async publish(result: HandResult | null = null): Promise<void> {
    if (result)
      await this.records.commitRound(String(this.context.spotId), result.handNumber, result.seats);
    for (const seat of this.state.snapshot.seats) {
      await this.actors.sendToActor(seat.playerId, this.view(seat.playerId)).submit();
      if (result) await this.actors.sendToActor(seat.playerId, result).submit();
    }
  }
  async deal(): Promise<void> {
    await this.publish();
    for (const seat of this.state.snapshot.seats)
      await this.actors.sendToActor(seat.playerId, this.handFor(seat.playerId)).submit();
  }
}
@Injectable()
export class TableRelocation implements ZLinkSpotRelocationAdapter<Table> {
  async capture(spot: Table): Promise<Uint8Array> {
    const table = serializeTableSnapshot(spot.state.snapshot);
    if (!spot.assignment) return table;
    return Buffer.from(
      JSON.stringify({
        table: Buffer.from(table).toString(),
        assignment: spot.assignment
      })
    );
  }
  async restore(spot: Table, payload: Uint8Array): Promise<void> {
    const snapshot = JSON.parse(Buffer.from(payload).toString()) as {
      table?: string;
      assignment?: RoomAssignment;
    };
    const table = snapshot.table === undefined ? payload : Buffer.from(snapshot.table);
    spot.state.snapshot = deserializeTableSnapshot(table);
    spot.assignment = snapshot.assignment;
  }
}
