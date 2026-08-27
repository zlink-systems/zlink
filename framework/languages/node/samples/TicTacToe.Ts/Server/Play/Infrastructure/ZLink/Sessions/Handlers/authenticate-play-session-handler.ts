import { Inject, Injectable } from '@nestjs/common';
import { ZLINK_ACTOR_MANAGER, ZLINK_CHANNEL_CLIENT } from '@zlink-systems/nestjs';
import { SampleNames } from '../../../../../Configuration/sample-settings';
import {
  AuthenticateReq,
  PacketNames,
  PlayerActorCreateReq,
  authenticatePlayerReq,
  authenticateRes
} from '../../../../../../Shared/Contracts/messages';
import {
  ZLinkPacket,
  type ZLinkActorManager,
  type ZLinkChannelClient,
  type ZLinkMessage,
  type ZLinkSessionContext,
  type ZLinkSessionDispatchContext
} from '@zlink-systems/framework';
import type { AuthenticatePlayerRes } from '../../../../../../Shared/Contracts/messages';

@Injectable()
@ZLinkPacket(PacketNames.authenticateReq)
// --8<-- [start:doc-session-auth]
class AuthenticatePlaySessionHandler {
  constructor(
    @Inject(ZLINK_ACTOR_MANAGER) private readonly actors: ZLinkActorManager,
    @Inject(ZLINK_CHANNEL_CLIENT) private readonly api: ZLinkChannelClient
  ) {}

  async handle(context: ZLinkSessionContext, _dispatch: ZLinkSessionDispatchContext, payload: ZLinkMessage): Promise<void> {
    const request = payload.decode(AuthenticateReq);
    const authenticated = await this.api
      .requestToChannel(
        SampleNames.apiChannel,
        authenticatePlayerReq(request.accessToken)
      )
      .submit<AuthenticatePlayerRes>();
    const created = await this.actors
      .getOrCreate(authenticated.player.actorId, SampleNames.playerActorType)
      .inMesh(SampleNames.playSpotNode)
      .request(new PlayerActorCreateReq(authenticated.player))
      .submit();
    if (created.status === 'rejected') {
      throw new Error(`Player actor '${authenticated.player.actorId}' creation was rejected.`);
    }
    const actorRef = created.actor;
    await context.actors.bindOrGet(actorRef);
    if (created.status === 'existing') {
      // Keep the exact ActorRef as server-only evidence. AuthenticateRes exposes
      // PlayerInfo only, so the client cannot choose or forge an Actor route.
      console.log(`tictactoe-lifecycle actor-bound actor=${actorRef.actorId}`);
    }
    context.client.reply(authenticateRes(authenticated.player)).submit();
  }
}
// --8<-- [end:doc-session-auth]

export { AuthenticatePlaySessionHandler };
