import { Injectable, Scope } from '@nestjs/common';
import {
  zlinkEntrySpotActorRequestHandler,
  zlinkEntrySpotActorSendHandler
} from '@zlink-systems/nestjs';
import {
  ZLinkFrameworkErrorKind,
  ZLinkFrameworkException,
  type ZLinkActor,
  type ZLinkActorContext,
  type ZLinkActorFactory,
  type ZLinkMessageContext
} from '@zlink-systems/framework';
import {
  NicknameChanged,
  PacketNames,
  type ChangeNickname,
  type GetPlayer,
  type PlayerInfo
} from '../../Shared/contracts';
import { LobbySpot } from '../Spots/lobby-spot';

// --8<-- [start:actor-class]
// A player is addressed by its own id and carries state that outlives any one
// connection. Like a room, its messages run one at a time. The scope is
// TRANSIENT because the Framework builds one instance per player.
@Injectable({ scope: Scope.TRANSIENT })
class Player implements ZLinkActor {
  readonly context!: ZLinkActorContext;

  private currentNickname = 'anonymous';

  get nickname(): string {
    return this.currentNickname;
  }

  rename(value: string): void {
    this.currentNickname = value;
  }
}
// --8<-- [end:actor-class]

// --8<-- [start:actor-factory]
// The Framework creates players through this factory rather than by calling a
// constructor, so dependencies can be injected here.
@Injectable()
class PlayerFactory implements ZLinkActorFactory<Player> {
  async create(context: ZLinkActorContext): Promise<Player> {
    const player = new Player();
    (player as { context: ZLinkActorContext }).context = context;
    return player;
  }
}
// --8<-- [end:actor-factory]

// A message addressed to a player runs inside the Spot the player currently
// occupies, so a handler receives both the Spot and the player.

// --8<-- [start:actor-handlers]
// --8<-- [start:actor-send-handler]
@zlinkEntrySpotActorSendHandler({
  entrySpot: () => LobbySpot,
  actor: () => Player,
  packetName: PacketNames.changeNickname
})
class ChangeNicknameHandler {
  async handle(
    _lobby: LobbySpot,
    player: Player,
    _context: ZLinkMessageContext,
    message: ChangeNickname
  ): Promise<void> {
    player.rename(message.nickname);

    // --8<-- [start:actor-push]
    // Pushes over the connection bound to this player. The same handler also runs
    // on an HTTP path with no bound connection, where push ends with InvalidOperation.
    // Rename is already complete, so only that failure is discarded.
    try {
      await player.context.boundSession.send(new NicknameChanged(player.nickname)).submit();
    } catch (error) {
      if (
        !(error instanceof ZLinkFrameworkException) ||
        error.kind !== ZLinkFrameworkErrorKind.InvalidOperation
      ) {
        throw error;
      }
    }
    // --8<-- [end:actor-push]
  }
}

// --8<-- [end:actor-send-handler]
// --8<-- [start:actor-request-handler]
// The return value is the reply. This handler only reads.
@zlinkEntrySpotActorRequestHandler({
  entrySpot: () => LobbySpot,
  actor: () => Player,
  packetName: PacketNames.getPlayer
})
class GetPlayerHandler {
  async handle(
    _lobby: LobbySpot,
    player: Player,
    _context: ZLinkMessageContext,
    _request: GetPlayer
  ): Promise<PlayerInfo> {
    return { playerId: String(player.context.actorId), nickname: player.nickname };
  }
}
// --8<-- [end:actor-request-handler]
// --8<-- [end:actor-handlers]

export { ChangeNicknameHandler, GetPlayerHandler, Player, PlayerFactory };
