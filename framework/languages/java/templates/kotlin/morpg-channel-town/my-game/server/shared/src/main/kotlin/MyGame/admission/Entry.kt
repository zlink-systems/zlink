package MyGame.admission

import MyGame.*
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.handlers.*
import systems.zlink.framework.kotlin.*
import systems.zlink.framework.spots.*

class Entry(override val context: ZLinkEntrySpotContext) : ZLinkSuspendingEntrySpot<Player>() {
    override suspend fun onJoinedActorSuspending(actor: Player) {}

    override suspend fun onLeaveActorSuspending(actor: Player) {}
}

@ZLinkHandlerGroup(Names.PLAYER)
class EntryActions {
    @ZLinkSpotActorSend
    suspend fun join(
        entry: Entry,
        actor: Player,
        dispatch: ZLinkMessageContext,
        request: JoinTownRequest,
    ) {
        require(request.playerId == actor.context.actorId()) { "Player identity mismatch" }
        actor.context.joinSpot(Names.townId(request.channel), request).defer()
    }

    @ZLinkSpotActorSend
    suspend fun pending(
        entry: Entry,
        actor: Player,
        context: ZLinkMessageContext,
        message: TownView,
    ) = actor.deliver(message.townId, message)
}
