package MyGame.region.infrastructure.zlink

import MyGame.*
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.handlers.*
import systems.zlink.framework.kotlin.*
import systems.zlink.framework.spots.*

class Entry(override val context: ZLinkEntrySpotContext, private val spots: ZLinkSpotManager) :
    ZLinkSuspendingEntrySpot<Player>() {
    override suspend fun onJoinedActorSuspending(actor: Player) {}

    override suspend fun onLeaveActorSuspending(actor: Player) {}

    suspend fun select(actor: Player, region: String, index: Int) {
        val id = Names.regionId(region, index)
        val created = spots.kotlin().getOrCreate(id, Names.REGION).inMesh(Names.MESH).await()
        check(created.state() != ZLinkSpotCreateState.REJECTED) { "Region creation rejected: $id" }
        actor.context.joinSpot(id, JoinRegion(region, index)).defer()
    }
}

@ZLinkHandlerGroup(Names.PLAYER)
class Enter {
    @ZLinkSpotActorSend
    suspend fun enter(
        entry: Entry,
        actor: Player,
        dispatch: ZLinkMessageContext,
        request: EnterRegionRequest,
    ) {
        actor.checkIdentity(request.playerId)
        entry.select(actor, request.region, Names.FIRST_REGION_INDEX)
    }

    @ZLinkSpotActorSend
    suspend fun next(
        entry: Entry,
        actor: Player,
        dispatch: ZLinkMessageContext,
        reply: RegionFull,
    ) {
        require(reply.reason == Names.REGION_FULL) { "Unknown join rejection" }
        entry.select(actor, reply.region, Math.incrementExact(reply.index))
    }
}
