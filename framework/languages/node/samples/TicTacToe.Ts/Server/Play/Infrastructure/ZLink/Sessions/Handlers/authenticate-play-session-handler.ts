import { Inject, Injectable } from '@nestjs/common';
import { ZLINK_ACTOR_MANAGER, ZLINK_ROUTE_CLIENT } from '@zlink-systems/nestjs';
import { SampleNames } from '../../../../../Configuration/sample-settings';
import {
  PacketNames,
  authenticatePlayerReq,
  authenticateRes
} from '../../../../../../Shared/Contracts/messages';
import {
  ZLinkPacket,
  type ZLinkActorManager,
  type ZLinkRouteClient,
  type ZLinkMessage,
  type ZLinkSessionContext,
  type ZLinkSessionDispatchContext
} from '@zlink-systems/framework';
import type { AuthenticatePlayerRes, AuthenticateReq } from '../../../../../../Shared/Contracts/messages';

@Injectable()
@ZLinkPacket(PacketNames.authenticateReq)
// --8<-- [start:doc-session-auth]
class AuthenticatePlaySessionHandler {
  constructor(
    @Inject(ZLINK_ACTOR_MANAGER) private readonly actors: ZLinkActorManager,
    @Inject(ZLINK_ROUTE_CLIENT) private readonly api: ZLinkRouteClient
  ) {}

  async handle(context: ZLinkSessionContext, _dispatch: ZLinkSessionDispatchContext, payload: ZLinkMessage): Promise<void> {
    const request = payload.decode<AuthenticateReq>(Object as never);
    const authenticated = await this.api
      .requestToChannel(
        SampleNames.apiChannel,
        authenticatePlayerReq(request.accessToken)
      )
      .submit<AuthenticatePlayerRes>();
    const created = await this.actors
      .getOrCreate(authenticated.player.actorId, SampleNames.playerActorType)
      .inMesh(SampleNames.playSpotNode)
      .request(authenticated.player)
      .submit();
    if (created.status === 'rejected') {
      throw new Error(`Player actor '${authenticated.player.actorId}' creation was rejected.`);
    }
    const actorRef = created.actor;
    await context.actors.bindOrGet(actorRef);
    context.client.reply(authenticateRes(authenticated.player)).submit();
  }
}
// --8<-- [end:doc-session-auth]

export { AuthenticatePlaySessionHandler };
