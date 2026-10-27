import { Inject, Injectable, Scope } from '@nestjs/common';
import { ZLINK_ACTOR_CLIENT, ZLINK_SPOT_MANAGER, zlinkSpotPacketHandler } from '@zlink-systems/nestjs';
import { ZLinkSpotCreateState, type ZLinkActorClient, type ZLinkSpot, type ZLinkSpotContext,
  type ZLinkSpotManager, type ZLinkSpotRelocationAdapter } from '@zlink-systems/framework';
import { Names, dungeonId } from '../shared/constants';
import { ActionRejected, PartyCommand, PartyCommandResult, DungeonPrepared, PrepareDungeon,
  JoinDungeon, DungeonMemberEntered, RunStarted, RunCompleted } from '../shared/messages';
import { Player } from '../shared/player';
import { PartyRun } from './party-domain';
@Injectable({ scope: Scope.TRANSIENT })
export class Party implements ZLinkSpot<Player> {
  readonly context!: ZLinkSpotContext<Player, Party>;
  private state = new PartyRun();
  constructor(@Inject(ZLINK_ACTOR_CLIENT) private readonly actors: ZLinkActorClient,
    @Inject(ZLINK_SPOT_MANAGER) private readonly spots: ZLinkSpotManager) {}
  async onActorJoin() { return { accepted: false }; }
  async onJoinedActor(): Promise<void> {}
  async onLeaveActor(): Promise<void> {}
  async command(c: PartyCommand): Promise<PartyCommandResult> {
    const rejected = c.join ? this.state.join(c.actorId) : c.enter ? this.state.enter(c.actorId)
      : this.state.members.includes(c.actorId) ? null : new ActionRejected('invalid-party');
    if (rejected) return new PartyCommandResult(null, rejected);
    if (c.enter && this.state.phase !== 'Active') {
      const run = this.state.runId!, target = dungeonId(run);
      const created = await this.spots.getOrCreate(target, Names.dungeon).inMesh(Names.mesh).submit();
      if (created.state === ZLinkSpotCreateState.Rejected) throw new Error('Dungeon creation rejected');
      const prepared = await this.context.outbound.requestToSpot(target,
        new PrepareDungeon(run, String(this.context.spotId), [...this.state.members])).submit<DungeonPrepared>();
      if (prepared.rejection) return new PartyCommandResult(null, prepared.rejection);
      if (prepared.dungeonId !== target) throw new Error('Dungeon identity mismatch');
      const rejection = this.state.prepared(prepared.runId);
      if (rejection) return new PartyCommandResult(null, rejection);
      for (const id of this.state.members.filter(id => !this.state.entered.includes(id)))
        await this.actors.sendToActor(id, new JoinDungeon(target, run)).submit();
    }
    if (c.join || c.enter) await this.broadcast();
    return new PartyCommandResult(this.state.view(String(this.context.spotId)));
  }
  async progress(run: string, actor: string | null, complete = false): Promise<PartyCommandResult> {
    const rejected = actor !== null ? this.state.arrived(run, actor) : complete
      ? this.state.completed(run) : this.state.started(run);
    if (rejected) return new PartyCommandResult(null, rejected);
    await this.broadcast();
    return new PartyCommandResult(this.state.view(String(this.context.spotId)));
  }
  private async broadcast(): Promise<void> {
    const view = this.state.view(String(this.context.spotId));
    for (const id of this.state.members) await this.actors.sendToActor(id, view).submit();
  }
  capture(): Uint8Array { return Buffer.from(JSON.stringify(this.state)); }
  restore(payload: Uint8Array): void {
    this.state = Object.assign(new PartyRun(), JSON.parse(Buffer.from(payload).toString('utf8')));
  }
}
@Injectable()
@zlinkSpotPacketHandler({ spot: () => Party, packetName: 'PartyCommand' })
export class Command {
  async handle(spot: Party, request: PartyCommand): Promise<PartyCommandResult> { return spot.command(request); }
}
@Injectable()
@zlinkSpotPacketHandler({ spot: () => Party, packetName: 'DungeonMemberEntered' })
export class MemberEntered {
  async handle(spot: Party, request: DungeonMemberEntered): Promise<PartyCommandResult> {
    return spot.progress(request.runId, request.actorId);
  }
}
@Injectable()
@zlinkSpotPacketHandler({ spot: () => Party, packetName: 'RunStarted' })
export class Started {
  async handle(spot: Party, request: RunStarted): Promise<PartyCommandResult> { return spot.progress(request.runId, null); }
}
@Injectable()
@zlinkSpotPacketHandler({ spot: () => Party, packetName: 'RunCompleted' })
export class Completed {
  async handle(spot: Party, request: RunCompleted): Promise<PartyCommandResult> { return spot.progress(request.runId, null, true); }
}
@Injectable()
export class PartyRelocation implements ZLinkSpotRelocationAdapter<Party> {
  async capture(spot: Party): Promise<Uint8Array> { return spot.capture(); }
  async restore(spot: Party, bytes: Uint8Array): Promise<void> { spot.restore(bytes); }
}
