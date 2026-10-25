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
import { DungeonView, DungeonPrepared, JoinDungeon, PrepareDungeon, PartyCommand, PartyCommandResult,
  DungeonMemberEntered, RunStarted, RunCompleted, ActionRejected } from '../shared/messages';
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
    const rejection = this.state.prepare(request, String(this.context.spotId));
    return new DungeonPrepared(String(this.context.spotId), request.runId, rejection);
  }
  async onActorJoin(id: string, payload: ZLinkMessage) {
    return {
      accepted: this.state.admit(id, payload.decode(JoinDungeon), String(this.context.spotId))
    };
  }
  async onJoinedActor(actor: Player): Promise<void> {
    this.state.arrived(actor.context.actorId);
    const entered = await this.context.outbound.requestToSpot(this.state.partyId,
      new DungeonMemberEntered(this.state.runId, actor.context.actorId)).submit<PartyCommandResult>();
    if (entered.rejection) throw new Error(entered.rejection.reason);
    if (this.state.phase === 'Active') {
      const started = await this.context.outbound.requestToSpot(this.state.partyId,
        new RunStarted(this.state.runId)).submit<PartyCommandResult>();
      if (started.rejection) throw new Error(started.rejection.reason);
      for (const id of this.state.members) await this.actors.sendToActor(id, this.view()).submit();
    }
  }
  async onLeaveActor(_actor: Player): Promise<void> {}
  async onDisconnectActor(_actor: Player): Promise<void> {}
  private view(): DungeonView {
    return new DungeonView(
      String(this.context.spotId),
      this.config.id,
      this.state.partyId,
      [...this.state.members],
      this.state.stage, this.state.runId, this.state.phase
    );
  }
  async show(actor: Player): Promise<void> {
    await actor.context.boundSession.send(this.view()).submit();
  }
  async advance(actor: Player): Promise<void> {
    const rejection = this.state.advance();
    if (rejection) { await actor.context.boundSession.send(rejection).submit(); return; }
    if (this.state.phase === 'Completed') {
      const completed = await this.context.outbound.requestToSpot(this.state.partyId,
        new RunCompleted(this.state.runId)).submit<PartyCommandResult>();
      if (completed.rejection) throw new Error(completed.rejection.reason);
    }
    const view = this.view();
    await actor.context.boundSession.send(view).submit();
    for (const id of this.state.members)
      if (id !== actor.context.actorId) await this.actors.sendToActor(id, view).submit();
  }
  async enter(actor: Player): Promise<void> {
    const result = await this.context.outbound.requestToSpot(this.state.partyId,
      new PartyCommand(actor.context.actorId, false, true)).submit<PartyCommandResult>();
    if (result.rejection) await actor.context.boundSession.send(Object.assign(new ActionRejected(), result.rejection)).submit();
  }
  capture(): Uint8Array {
    return Buffer.from(JSON.stringify(this.state));
  }
  restore(payload: Uint8Array): void {
    this.state = Object.assign(
      new DungeonState(),
      JSON.parse(Buffer.from(payload).toString('utf8'))
    );
  }
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
