import { Inject, Injectable, Scope } from '@nestjs/common';
import { ZLINK_ACTOR_CLIENT, ZLINK_SPOT_MANAGER } from '@zlink-systems/nestjs';
import {
  ZLinkSpotCreateState,
  type ZLinkActorClient,
  type ZLinkEntrySpot,
  type ZLinkEntrySpotContext,
  type ZLinkSpot,
  type ZLinkSpotContext,
  type ZLinkSpotManager,
  type ZLinkMessage,
  type ZLinkSpotRelocationAdapter
} from '@zlink-systems/framework';
import { NODE_CONFIG, type NodeConfig } from '../../../shared/config';
import { Names, regionId } from '../../../shared/constants';
import {
  JoinRegion,
  RegionFull,
  RegionView,
  DungeonOpened,
  DungeonInfoRequest,
  type MoveRequest
} from '../../../shared/messages';
import { Rejection } from '../../../shared/constants';
import { Positions } from '../../domain/positions';
import { Player } from './player';
@Injectable()
export class Entry implements ZLinkEntrySpot<Player> {
  readonly context!: ZLinkEntrySpotContext<Player, Entry>;
  constructor(@Inject(ZLINK_SPOT_MANAGER) private readonly spots: ZLinkSpotManager) {}
  async onJoinedActor(_actor: Player): Promise<void> {}
  async onLeaveActor(_actor: Player): Promise<void> {}
  async select(actor: Player, region: string, index: number): Promise<void> {
    if (!region.trim() || !Number.isSafeInteger(index) || index < 1)
      throw new Error('Invalid region selection');
    const id = regionId(region, index);
    const result = await this.spots.getOrCreate(id, Names.region).inMesh(Names.mesh).submit();
    if (result.state === ZLinkSpotCreateState.Rejected)
      throw new Error(`Region creation rejected: ${id}`);
    actor.context.joinSpot(id, new JoinRegion(region, index)).defer();
  }
}
@Injectable({ scope: Scope.TRANSIENT })
export class Region implements ZLinkSpot<Player> {
  readonly context!: ZLinkSpotContext<Player, Region>;
  private readonly positions = new Positions();
  constructor(
    @Inject(NODE_CONFIG) private readonly config: NodeConfig,
    @Inject(ZLINK_ACTOR_CLIENT) private readonly actors: ZLinkActorClient,
    @Inject(ZLINK_SPOT_MANAGER) private readonly spots: ZLinkSpotManager
  ) {}
  async onActorJoin(id: string, payload: ZLinkMessage) {
    const join = payload.decode(JoinRegion);
    if (regionId(join.region, join.index) !== String(this.context.spotId))
      throw new Error('Region join identity mismatch');
    return this.positions.admit(id, this.config.capacity)
      ? { accepted: true }
      : { accepted: false, reply: new RegionFull(Rejection.regionFull, join.region, join.index) };
  }
  async onJoinedActor(_actor: Player): Promise<void> {
    await this.broadcast();
  }
  async onLeaveActor(actor: Player): Promise<void> {
    this.positions.remove(actor.context.actorId);
    await this.broadcast();
  }
  // Membership and position survive a disconnected Session until explicit leave.
  async onDisconnectActor(_actor: Player): Promise<void> {}
  private view(): RegionView {
    return new RegionView(String(this.context.spotId), this.config.id, this.positions.view());
  }
  async show(actor: Player): Promise<void> {
    await actor.context.boundSession.send(this.view()).submit();
  }
  private async broadcast(): Promise<void> {
    const view = this.view();
    for (const id of this.positions.ids()) await this.actors.sendToActor(id, view).submit();
  }
  async move(actor: Player, request: MoveRequest): Promise<void> {
    this.positions.move(actor.context.actorId, request.x, request.sequence);
    await this.broadcast();
  }
  async open(actor: Player, dungeonId: string): Promise<void> {
    if (!dungeonId.trim()) throw new Error('Dungeon ID is required');
    const result = await this.spots
      .getOrCreate(dungeonId, Names.dungeon)
      .inMesh(Names.mesh)
      .submit();
    if (result.state === ZLinkSpotCreateState.Rejected)
      throw new Error('Dungeon creation rejected');
    const reply = await this.context.outbound
      .requestToSpot(dungeonId, new DungeonInfoRequest())
      .submit<DungeonOpened>();
    await actor.context.boundSession.send(Object.assign(new DungeonOpened(), reply)).submit();
  }
  capture(): Uint8Array {
    return Buffer.from(JSON.stringify(this.positions.snapshot()));
  }
  restore(payload: Uint8Array): void {
    this.positions.restore(JSON.parse(Buffer.from(payload).toString('utf8')));
  }
}
@Injectable()
export class RegionRelocation implements ZLinkSpotRelocationAdapter<Region> {
  async capture(spot: Region): Promise<Uint8Array> {
    return spot.capture();
  }
  async restore(spot: Region, payload: Uint8Array): Promise<void> {
    spot.restore(payload);
  }
}
