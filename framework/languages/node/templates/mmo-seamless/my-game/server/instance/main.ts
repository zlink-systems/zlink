import { Inject, Injectable, Scope } from "@nestjs/common";
import { zlinkSpotPacketHandler } from "@zlink-systems/nestjs";
import type {
  ZLinkSpot,
  ZLinkSpotContext,
  ZLinkSpotRelocationAdapter,
  ZLinkSpotRequestHandler,
  ZLinkMessageContext,
} from "@zlink-systems/framework";
import { runHost, failHost } from "../shared/hosting";
import { NODE_CONFIG, type NodeConfig } from "../shared/config";
import { Names, PacketNames } from "../shared/constants";
import { OpenDungeonRequest, DungeonOpened } from "../shared/messages";
// A Dungeon is a User Spot; the Framework Instance Spot has a different contract.
@Injectable({ scope: Scope.TRANSIENT })
class Dungeon implements ZLinkSpot {
  readonly context!: ZLinkSpotContext;
  async onActorJoin() {
    return { accepted: false };
  }
  async onJoinedActor(): Promise<void> {}
  async onLeaveActor(): Promise<void> {}
}
@Injectable()
@zlinkSpotPacketHandler({
  spot: () => Dungeon,
  packetName: PacketNames.openDungeonRequest,
})
class DungeonOpen
  implements ZLinkSpotRequestHandler<Dungeon, OpenDungeonRequest, DungeonOpened>
{
  constructor(@Inject(NODE_CONFIG) private readonly config: NodeConfig) {}
  async handle(
    spot: Dungeon,
    _request: OpenDungeonRequest,
    _context: ZLinkMessageContext,
  ): Promise<DungeonOpened> {
    return new DungeonOpened(String(spot.context.spotId), this.config.id);
  }
}
@Injectable()
class DungeonRelocation implements ZLinkSpotRelocationAdapter<Dungeon> {
  async capture(_spot: Dungeon): Promise<Uint8Array> {
    return new Uint8Array();
  }
  async restore(_spot: Dungeon, _payload: Uint8Array): Promise<void> {}
}
runHost((_builder, _config, mesh) => {
  mesh
    .objects()
    .server()
    .addSpotFactory(Names.dungeon, Dungeon, (f) =>
      f.preserveStateWith(DungeonRelocation),
    );
  return {
    providers: [Dungeon, DungeonOpen, DungeonRelocation],
    listener: { kind: "routeMesh", name: Names.mesh },
    ready: (status) => status.placement.isAvailable,
  };
}).catch(failHost);
