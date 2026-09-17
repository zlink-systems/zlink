package systems.zlink.tutorial.server.spots

import systems.zlink.framework.kotlin.ZLinkSuspendingEntrySpot
import systems.zlink.framework.kotlin.addHandler
import systems.zlink.framework.spots.ZLinkEntrySpotContext
import systems.zlink.tutorial.server.actors.ChangeNicknameHandler
import systems.zlink.tutorial.server.actors.GetPlayerHandler
import systems.zlink.tutorial.server.actors.Player

// --8<-- [start:entry-spot]
// Every new player lands here before joining a room, and returns here after
// leaving one. A node that hosts players registers exactly one of these.
class LobbySpot(
    override val context: ZLinkEntrySpotContext,
) : ZLinkSuspendingEntrySpot<Player>() {

    init {
        // --8<-- [start:actor-handler-register]
        // The handlers for a player's messages are registered on the Spot the
        // player occupies, which is this lobby.
        context.handlers().addHandler<ChangeNicknameHandler>()
        context.handlers().addHandler<GetPlayerHandler>()
        // --8<-- [end:actor-handler-register]
    }

    override suspend fun onJoinedActorSuspending(actor: Player) {
    }

    override suspend fun onLeaveActorSuspending(actor: Player) {
    }
}
// --8<-- [end:entry-spot]
