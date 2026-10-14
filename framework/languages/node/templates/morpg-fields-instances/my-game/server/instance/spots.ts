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
import { DungeonView, DungeonPrepared, JoinDungeon, PrepareDungeon } from '../shared/messages';
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
    this.state.prepare(request, String(this.context.spotId));
    return new DungeonPrepared(String(this.context.spotId));
  }
  async onActorJoin(id: string, payload: ZLinkMessage) {
    return {
      accepted: this.state.admit(id, payload.decode(JoinDungeon), String(this.context.spotId))
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
  async advance(actor: Player): Promise<void> {
    this.state.advance();
    const view = this.view();
    await actor.context.boundSession.send(view).submit();
    for (const id of this.state.members)
      if (id !== actor.context.actorId) await this.actors.sendToActor(id, view).submit();
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
