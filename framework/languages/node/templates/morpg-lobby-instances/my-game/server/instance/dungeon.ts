import { Inject, Injectable, Scope } from '@nestjs/common';
import { ZLINK_ACTOR_CLIENT } from '@zlink-systems/nestjs';
import type {
  ZLinkActorClient,
  ZLinkMessage,
  ZLinkSpot,
  ZLinkSpotContext,
  ZLinkSpotRelocationAdapter
} from '@zlink-systems/framework';
import { NODE_CONFIG, type NodeConfig } from '../shared/config';
import { Names, Rejection } from '../shared/constants';
import { DungeonView, DungeonPrepared, JoinDungeon, DungeonMemberEntered, RunStarted, RunCompleted, EnterPartyRun, RunPhase, ActionRejected, type PrepareDungeon } from '../shared/messages';
import { Player } from '../shared/player';
import { DungeonState } from './domain';
@Injectable({ scope: Scope.TRANSIENT })
export class Dungeon implements ZLinkSpot<Player> {
  readonly context!: ZLinkSpotContext<Player, Dungeon>;
  private state = new DungeonState();
  constructor(
    @Inject(NODE_CONFIG) private readonly config: NodeConfig,
    @Inject(ZLINK_ACTOR_CLIENT) private readonly actors: ZLinkActorClient
  ) {}
  prepare(request: PrepareDungeon): DungeonPrepared {
    if (Names.dungeonPrefix + request.runId !== String(this.context.spotId) || this.state.prepare(request)) throw new Error(Rejection.runMismatch);
    return new DungeonPrepared(String(this.context.spotId), request.runId);
  }
  async onActorJoin(id: string, payload: ZLinkMessage) {
    return { accepted: this.state.admit(id, payload.decode(JoinDungeon), String(this.context.spotId)) };
  }
  async onJoinedActor(actor: Player): Promise<void> {
    const started = this.state.join(actor.context.actorId);
    await this.context.outbound.sendToSpot(this.state.partyId, new DungeonMemberEntered(this.state.runId!, actor.context.actorId)).submit();
    if (started) await this.context.outbound.sendToSpot(this.state.partyId, new RunStarted(this.state.runId!)).submit();
    await this.broadcast();
  }
  async onLeaveActor(_actor: Player): Promise<void> {}
  private view(): DungeonView {
    return new DungeonView(
      String(this.context.spotId),
      this.config.id,
      this.state.partyId,
      this.state.members,
      this.state.stage, this.state.runId, this.state.phase
    );
  }
  async show(actor: Player): Promise<void> {
    await actor.context.boundSession.send(this.view()).submit();
  }
  async advance(actor: Player): Promise<void> {
    const reason = this.state.advance();
    if (reason) { await actor.context.boundSession.send(new ActionRejected(reason)).submit(); return; }
    if (this.state.phase === RunPhase.Completed) await this.context.outbound.sendToSpot(this.state.partyId, new RunCompleted(this.state.runId!)).submit();
    await this.broadcast();
  }
  async enter(actor: Player): Promise<void> { await this.context.outbound.sendToSpot(this.state.partyId, new EnterPartyRun(actor.context.actorId)).submit(); }
  private async broadcast(): Promise<void> {
    const view = this.view(); for (const id of this.state.joined) await this.actors.sendToActor(id, view).submit();
  }
  capture(): Uint8Array { return Buffer.from(this.state.capture()); }
  restore(payload: Uint8Array): void { this.state = DungeonState.restore(Buffer.from(payload).toString('utf8')); }

}
@Injectable()
export class DungeonRelocation implements ZLinkSpotRelocationAdapter<Dungeon> {
  async capture(spot: Dungeon): Promise<Uint8Array> {
    return spot.capture();
  }
  async restore(spot: Dungeon, payload: Uint8Array): Promise<void> {
    spot.restore(payload);
  }
}
