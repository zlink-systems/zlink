import { Inject } from '@nestjs/common';
import {
  ZLINK_ACTOR_MANAGER,
  ZLINK_ROUTE_MESH_RUNTIME,
  zlinkEntrySpotPacketHandler
} from '@zlink-systems/nestjs';
import { SampleNames } from '../../../../Configuration/sample-names';
import { PacketNames } from '../../../../../Shared/Contracts/messages';
import {
  EnsurePlayerActorReq as GeneratedEnsurePlayerActorReq,
  EnsurePlayerActorRes
} from '../../../../../Shared/Contracts/bingo-messages.generated';
import type { ZLinkActorManager, ZLinkRouteMeshRuntime, ZLinkSpotRequestHandler } from '@zlink-systems/framework';
import { BingoEntrySpot } from '../Spots/EntrySpot/bingo-entry-spot';
import type {
  EnsurePlayerActorReq
} from '../../../../../Shared/Contracts/messages';

@zlinkEntrySpotPacketHandler({
  entrySpot: () => BingoEntrySpot,
  packetName: PacketNames.ensurePlayerActorReq
})
class EnsurePlayerActorHandler implements
  ZLinkSpotRequestHandler<BingoEntrySpot, EnsurePlayerActorReq, EnsurePlayerActorRes> {
  constructor(
    @Inject(ZLINK_ACTOR_MANAGER) private readonly actorManager: ZLinkActorManager,
    @Inject(ZLINK_ROUTE_MESH_RUNTIME) private readonly routeMeshRuntime: ZLinkRouteMeshRuntime
  ) {}

  async handle(_spot: BingoEntrySpot, request: EnsurePlayerActorReq): Promise<EnsurePlayerActorRes> {
    if (!this.routeMeshRuntime.isReady(SampleNames.roomSpotNode)) {
      throw new Error('Draining Play node does not accept new actors.');
    }
    console.log(`play-ensure-actor request actor=${request.actorId}`);
    const result = await this.actorManager
      .getOrCreate(request.actorId, SampleNames.playerActorType)
      .inMesh(SampleNames.roomSpotNode)
      .request(new GeneratedEnsurePlayerActorReq({
        actorId: request.actorId,
        displayName: request.displayName
      }))
      .submit();
    if (result.status === 'rejected') throw new Error('Player Actor creation was rejected.');
    console.log(`play-ensure-actor ready actor=${result.actor.actorId}`);
    return new EnsurePlayerActorRes({
      actorId: result.actor.actorId,
      actorType: SampleNames.playerActorType
    });
  }
}

export { EnsurePlayerActorHandler };
