package systems.zlink.tutorial.server.actors

import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind
import systems.zlink.framework.errors.ZLinkFrameworkException
import systems.zlink.framework.kotlin.ZLinkSuspendingEntrySpotActorRequestHandler
import systems.zlink.framework.kotlin.ZLinkSuspendingEntrySpotActorSendHandler
import systems.zlink.framework.kotlin.kotlin
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
        // Pushes over the connection bound to this player. The same handler also runs
        // on an HTTP path with no bound connection, where push ends with InvalidOperation.
        // Rename is already complete, so only that failure is discarded.
        try {
            actor.context().boundSession().kotlin().send(NicknameChanged(actor.nickname)).await()
        } catch (error: ZLinkFrameworkException) {
            if (error.kind() != ZLinkFrameworkErrorKind.INVALID_OPERATION) {
                throw error
            }
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
