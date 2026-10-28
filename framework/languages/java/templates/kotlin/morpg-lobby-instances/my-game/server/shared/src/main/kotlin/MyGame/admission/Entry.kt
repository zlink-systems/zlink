package MyGame.admission

import MyGame.*
import systems.zlink.framework.kotlin.*
import systems.zlink.framework.spots.*

class Entry(override val context: ZLinkEntrySpotContext, private val spots: ZLinkSpotManager) :
    ZLinkSuspendingEntrySpot<Player>() {
    override suspend fun onJoinedActorSuspending(actor: Player) {}

    override suspend fun onLeaveActorSuspending(actor: Player) {}

    suspend fun join(actor: Player, partyId: String, create: Boolean) {
        if (partyId.isBlank() || (!create && spots.find(partyId).await().isEmpty)) {
            actor.context.boundSession().kotlin().send(ActionRejected(Rules.INVALID_PARTY)).await()
            return
        }
        val id = partyId
        if (create) {
            val result = spots.kotlin().getOrCreate(id, Names.PARTY).inMesh(Names.MESH).await()
            check(result.state() != ZLinkSpotCreateState.REJECTED) { "Party creation rejected" }
        }
        actor.context.joinSpot(id, PartyJoin(partyId, create)).defer()
    }
}
