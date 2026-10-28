import { Inject, Injectable, Scope } from '@nestjs/common';
import { ZLINK_ACTOR_CLIENT } from '@zlink-systems/nestjs';
import type {
  ZLinkActorClient,
  ZLinkSpot,
  ZLinkSpotContext,
  ZLinkMessage,
  ZLinkSpotRelocationAdapter
} from '@zlink-systems/framework';
import { NODE_CONFIG, type NodeConfig } from '../shared/config';
import { JoinDungeon } from '../shared/messages';
import {
  ActionRejected,
  DungeonPrepared,
  DungeonView,
  PartyViewRequest,
  PrepareDungeon,
  RunCompleted,
  RunStarted,
  RunPhase
} from '../shared/messages';
import { Player } from '../shared/player';
import { DungeonState, type DungeonSnapshot } from './dungeon-state';
@Injectable({ scope: Scope.TRANSIENT })
export class Dungeon implements ZLinkSpot<Player> {
  readonly context!: ZLinkSpotContext<Player, Dungeon>;
  private state = new DungeonState();
  constructor(
    @Inject(NODE_CONFIG) private readonly config: NodeConfig,
    @Inject(ZLINK_ACTOR_CLIENT) private readonly actors: ZLinkActorClient
  ) {}
  prepare(request: PrepareDungeon): DungeonPrepared {
    const dungeonId = String(this.context.spotId);
    const reason = this.state.prepare(dungeonId, request);
    return new DungeonPrepared(dungeonId, request.runId, reason);
  }
  async onActorJoin(actorId: string, request: ZLinkMessage) {
    const join = request.decode(JoinDungeon);
    return {
      accepted: this.state.accepts(actorId, String(this.context.spotId), join)
    };
  }
  async onJoinedActor(actor: Player): Promise<void> {
    if (this.state.join(actor.context.actorId))
      await this.context.outbound
        .sendToSpot(this.state.ownerSpotId, new RunStarted(this.state.runId))
        .submit();
    await this.broadcast();
  }
  async onLeaveActor(_actor: Player): Promise<void> {}
  async onDisconnectActor(_actor: Player): Promise<void> {}
  private view(): DungeonView {
    return new DungeonView(
      String(this.context.spotId),
      this.config.id,
      this.state.partyId,
      [...this.state.members],
      this.state.stage,
      this.state.runId,
      this.state.phase
    );
  }
  async show(actor: Player): Promise<void> {
    await actor.context.boundSession.send(this.view()).submit();
  }
  async showWithParty(actor: Player): Promise<void> {
    await this.context.outbound
      .sendToSpot(this.state.ownerSpotId, new PartyViewRequest(actor.context.actorId))
      .submit();
    await this.show(actor);
  }
  private async broadcast(): Promise<void> {
    const view = this.view();
    for (const id of this.state.joined) await this.actors.sendToActor(id, view).submit();
  }
  async advance(actor: Player): Promise<void> {
    const reason = this.state.advance();
    if (reason !== null) {
      await actor.context.boundSession.send(new ActionRejected(reason)).submit();
      return;
    }
    await this.broadcast();
    if (this.state.phase === RunPhase.Completed)
      await this.context.outbound
        .sendToSpot(this.state.ownerSpotId, new RunCompleted(this.state.runId))
        .submit();
  }
  capture(): DungeonSnapshot {
    return this.state.capture();
  }
  restore(state: DungeonSnapshot): void {
    this.state.restore(state);
  }
}
@Injectable()
export class DungeonRelocation implements ZLinkSpotRelocationAdapter<Dungeon> {
  async capture(spot: Dungeon): Promise<Uint8Array> {
    return Buffer.from(JSON.stringify(spot.capture()));
  }
  async restore(spot: Dungeon, payload: Uint8Array): Promise<void> {
    spot.restore(JSON.parse(Buffer.from(payload).toString('utf8')) as DungeonSnapshot);
  }
}
