import { Inject, Injectable } from '@nestjs/common';
import {
  ZLINK_ACTOR_MANAGER,
  ZLINK_CHANNEL_CLIENT,
} from '@zlink-systems/nestjs';
import { SampleNames } from '../../../Configuration/sample-names';
import {
  AuthenticatePlayerReq,
  AuthenticateRes,
  EnsurePlayerActorReq
} from '../../../../Shared/Contracts/bingo-messages.generated';
import { PacketNames } from '../../../../Shared/Contracts/messages';
import {
  ZLinkPacket,
  type ZLinkActorManager,
  type ZLinkChannelClient,
  type ZLinkMessage,
  type ZLinkSessionContext,
  type ZLinkSessionDispatchContext
} from '@zlink-systems/framework';
import type {
  AuthenticatePlayerRes,
  AuthenticateReq,
} from '../../../../Shared/Contracts/messages';

@Injectable()
@ZLinkPacket(PacketNames.authenticateReq)
class SessionAuthenticator {
  constructor(
    @Inject(ZLINK_CHANNEL_CLIENT) private readonly zlinkClient: ZLinkChannelClient,
    @Inject(ZLINK_ACTOR_MANAGER) private readonly actors: ZLinkActorManager
  ) {}

  async handle(
    context: ZLinkSessionContext,
    _dispatch: ZLinkSessionDispatchContext,
    payload: ZLinkMessage
  ): Promise<void> {
    const request = payload.decode<AuthenticateReq>(Object as never);
    console.log(`session-auth request api actor=${request.accessToken}`);
    const authenticated = await this.zlinkClient
        .requestToChannel(
          SampleNames.apiChannel,
          new AuthenticatePlayerReq({ accessToken: request.accessToken })
        )
        .timeout(500)
        .submit<AuthenticatePlayerRes>();
    console.log(`session-auth api accepted=${authenticated.accepted} actor=${authenticated.actorId ?? '-'}`);

    if (
      !authenticated.accepted ||
      authenticated.actorId === null ||
      authenticated.actorId.length === 0 ||
      authenticated.displayName === null ||
      authenticated.displayName.length === 0
    ) {
      throw new Error(authenticated.reason ?? 'Player authentication failed.');
    }

    console.log(`session-auth ensure actor=${authenticated.actorId}`);
    const ensured = await this.actors
      .getOrCreate(authenticated.actorId, SampleNames.playerActorType)
      .inMesh(SampleNames.roomSpotNode)
      .request(new EnsurePlayerActorReq({
        actorId: authenticated.actorId,
        displayName: authenticated.displayName
      }))
      .timeout(500)
      .submit(AbortSignal.timeout(500));
    if (ensured.status === 'rejected') throw new Error('Player Actor creation was rejected.');
    console.log(`session-auth ensured actor=${ensured.actor.actorId}`);

    console.log(`session-auth bind actor=${ensured.actor.actorId}`);
    try {
      await context.actors.bindOrGet(ensured.actor);
    } catch (error) {
      console.log(`session-auth bind failed=${error instanceof Error ? error.stack ?? error.message : String(error)}`);
      throw error;
    }
    console.log(`session-auth bound actor=${ensured.actor.actorId}`);
    await context.client.reply(new AuthenticateRes({
      actorId: ensured.actor.actorId,
      displayName: authenticated.displayName
    })).submit();
    console.log(`session-auth replied actor=${ensured.actor.actorId}`);
  }
}

export { SessionAuthenticator };
