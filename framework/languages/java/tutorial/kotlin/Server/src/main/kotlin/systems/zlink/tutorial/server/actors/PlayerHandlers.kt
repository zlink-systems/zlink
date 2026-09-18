package systems.zlink.tutorial.server.actors

import kotlinx.coroutines.future.await
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.kotlin.ZLinkSuspendingEntrySpotActorRequestHandler
import systems.zlink.framework.kotlin.ZLinkSuspendingEntrySpotActorSendHandler
import systems.zlink.tutorial.server.spots.LobbySpot
import systems.zlink.tutorial.shared.ChangeNickname
import systems.zlink.tutorial.shared.GetPlayer
import systems.zlink.tutorial.shared.NicknameChanged
import systems.zlink.tutorial.shared.PlayerInfo

// A message addressed to a player runs inside the Spot the player currently
// occupies, so a handler receives both the Spot and the player.

// --8<-- [start:actor-handlers]
// --8<-- [start:actor-send-handler]
class ChangeNicknameHandler :
    ZLinkSuspendingEntrySpotActorSendHandler<LobbySpot, Player, ChangeNickname> {
    override suspend fun handle(
        entrySpot: LobbySpot,
        actor: Player,
        context: ZLinkMessageContext,
        message: ChangeNickname,
    ) {
        actor.rename(message.nickname)

        // --8<-- [start:actor-push]
        // Reaches the connection bound to this player. The same handler also runs
        // for a player nobody is connected to -- the HTTP path of the Actor step --
        // and on this binding that push fails instead of doing nothing, so the
        // failure is dropped rather than failing the rename.
        runCatching {
            actor.context().boundSession()
                .send(NicknameChanged(actor.nickname))
                .submit()
                .await()
        }
        // --8<-- [end:actor-push]
    }
}

// --8<-- [end:actor-send-handler]
// The return value is the reply. This handler only reads.
// --8<-- [start:actor-request-handler]
class GetPlayerHandler :
    ZLinkSuspendingEntrySpotActorRequestHandler<LobbySpot, Player, GetPlayer, PlayerInfo> {
    override suspend fun handle(
        entrySpot: LobbySpot,
        actor: Player,
        context: ZLinkMessageContext,
        request: GetPlayer,
    ): PlayerInfo = PlayerInfo(actor.context().actorId(), actor.nickname)
}
// --8<-- [end:actor-request-handler]
// --8<-- [end:actor-handlers]
