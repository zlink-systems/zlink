import { Inject, Injectable, Scope } from '@nestjs/common';
import { zlinkSpotPacketHandler } from '@zlink-systems/nestjs';
import type {
  ZLinkSpot,
  ZLinkSpotContext,
  ZLinkMessageContext,
  ZLinkSpotRequestHandler
} from '@zlink-systems/framework';
import { NODE_CONFIG, type NodeConfig } from '../../../shared/config';
import { PacketNames } from '../../../shared/constants';
import { DungeonOpened, DungeonInfoRequest } from '../../../shared/messages';
import { Player } from '../../../region/infrastructure/zlink/player';
@Injectable({ scope: Scope.TRANSIENT })
export class Dungeon implements ZLinkSpot<Player> {
  readonly context!: ZLinkSpotContext<Player, Dungeon>;
  constructor(@Inject(NODE_CONFIG) private readonly config: NodeConfig) {}
  async onActorJoin() {
    return { accepted: false };
  }
  async onJoinedActor(_actor: Player): Promise<void> {}
  async onLeaveActor(_actor: Player): Promise<void> {}
  info(): DungeonOpened {
    return new DungeonOpened(String(this.context.spotId), this.config.id);
  }
}
@Injectable()
@zlinkSpotPacketHandler({ spot: () => Dungeon, packetName: PacketNames.dungeonInfoRequest })
export class DungeonInfo
  implements ZLinkSpotRequestHandler<Dungeon, DungeonInfoRequest, DungeonOpened>
{
  async handle(
    spot: Dungeon,
    _request: DungeonInfoRequest,
    _context: ZLinkMessageContext
  ): Promise<DungeonOpened> {
    return spot.info();
  }
}
