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
import { Game, dungeonId } from '../shared/constants';
import { PrepareDungeon, DungeonPrepared, JoinDungeon, DungeonView } from '../shared/messages';
import { Player } from '../shared/player';
interface DungeonState {
  partyId: string;
  members: string[];
  stage: number;
}
@Injectable({ scope: Scope.TRANSIENT })
export class Dungeon implements ZLinkSpot<Player> {
  readonly context!: ZLinkSpotContext<Player, Dungeon>;
  private state: DungeonState = { partyId: '', members: [], stage: 0 };
  constructor(
    @Inject(NODE_CONFIG) private readonly config: NodeConfig,
    @Inject(ZLINK_ACTOR_CLIENT) private readonly actors: ZLinkActorClient
  ) {}
  prepare(request: PrepareDungeon): DungeonPrepared {
    if (
      this.state.partyId ||
      !request.partyId.trim() ||
      !request.members.length ||
      request.members.length > Game.maximumPartyMembers ||
      new Set(request.members).size !== request.members.length ||
      request.members.some((id) => !id.trim()) ||
      dungeonId(request.partyId) !== String(this.context.spotId)
    )
      throw new Error('Invalid dungeon preparation');
    this.state.partyId = request.partyId;
    this.state.members = [...request.members];
    return new DungeonPrepared(String(this.context.spotId));
  }
  async onActorJoin(actorId: string, request: ZLinkMessage) {
    const join = request.decode(JoinDungeon);
    return {
      accepted:
        join.dungeonId === String(this.context.spotId) &&
        join.partyId === this.state.partyId &&
        this.state.members.includes(actorId)
    };
  }
  async onJoinedActor(actor: Player): Promise<void> {
    await this.actors.sendToActor(actor.context.actorId, this.view()).submit();
  }
  async onLeaveActor(_actor: Player): Promise<void> {}
  async onDisconnectActor(_actor: Player): Promise<void> {}
  private view(): DungeonView {
    return new DungeonView(
      String(this.context.spotId),
      this.config.id,
      this.state.partyId,
      [...this.state.members],
      this.state.stage
    );
  }
  async show(actor: Player): Promise<void> {
    await actor.context.boundSession.send(this.view()).submit();
  }
  async advance(): Promise<void> {
    if (!Number.isSafeInteger(this.state.stage + 1)) throw new Error('Dungeon stage exhausted');
    this.state.stage++;
    const view = this.view();
    for (const id of this.state.members) await this.actors.sendToActor(id, view).submit();
  }
  capture(): DungeonState {
    return this.state;
  }
  restore(state: DungeonState): void {
    this.state = state;
  }
}
@Injectable()
export class DungeonRelocation implements ZLinkSpotRelocationAdapter<Dungeon> {
  async capture(spot: Dungeon): Promise<Uint8Array> {
    return Buffer.from(JSON.stringify(spot.capture()));
  }
  async restore(spot: Dungeon, payload: Uint8Array): Promise<void> {
    spot.restore(JSON.parse(Buffer.from(payload).toString('utf8')) as DungeonState);
  }
}
