import { Inject, Injectable } from '@nestjs/common';
import { ZLINK_ACTOR_MANAGER } from '@zlink-systems/nestjs';
import { ZLinkPacket } from '@zlink-systems/framework';
import type {
  ZLinkActorManager,
  ZLinkMessage,
  ZLinkSessionContext,
  ZLinkSessionDispatchContext
} from '@zlink-systems/framework';
import {
  Authenticate,
  Authenticated,
  CreatePlayer,
  PacketNames,
  TutorialNames
} from '../../Shared/contracts';

// --8<-- [start:session-actor-bind]
// Ties this connection to one player. After this, packets without a session
// handler reach that player, and the player can push to this connection.
@Injectable()
@ZLinkPacket(PacketNames.authenticate)
class AuthenticateHandler {
  constructor(@Inject(ZLINK_ACTOR_MANAGER) private readonly players: ZLinkActorManager) {}

  async handle(
    context: ZLinkSessionContext,
    _dispatch: ZLinkSessionDispatchContext,
    payload: ZLinkMessage
  ): Promise<void> {
    const message = payload.decode(Authenticate);

    // A returning client finds its existing player rather than a new one.
    const result = await this.players
      .getOrCreate(message.playerId, TutorialNames.playerActorType)
      .inMesh(TutorialNames.mesh)
      .request(new CreatePlayer(message.playerId))
      .timeout(10_000)
      .submit();

    if (result.status === 'rejected') {
      throw new Error(`Player '${message.playerId}' creation was rejected.`);
    }

    const bound = await context.actors.bindOrGet(result.actor);

    await context.client.reply(new Authenticated(bound.actorId)).submit();
  }
}
// --8<-- [end:session-actor-bind]

export { AuthenticateHandler };
