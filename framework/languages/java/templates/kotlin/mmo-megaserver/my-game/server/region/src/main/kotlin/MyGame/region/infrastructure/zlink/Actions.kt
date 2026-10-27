package MyGame.region.infrastructure.zlink

import MyGame.*
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.handlers.*
import systems.zlink.framework.kotlin.*
import systems.zlink.framework.spots.*

@ZLinkHandlerGroup(Names.PLAYER)
class Actions(private val spots: ZLinkSpotManager) {
    @ZLinkSpotActorSend
    suspend fun reconnect(
        spot: Region,
        actor: Player,
        dispatch: ZLinkMessageContext,
        request: EnterRegionRequest,
    ) {
        actor.checkIdentity(request.playerId)
        spot.show(actor)
    }

    @ZLinkSpotActorSend
    suspend fun query(
        spot: Region,
        actor: Player,
        dispatch: ZLinkMessageContext,
        request: ViewRequest,
    ) {
        spot.show(actor)
    }

    @ZLinkSpotActorSend
    suspend fun move(
        spot: Region,
        actor: Player,
        dispatch: ZLinkMessageContext,
        request: MoveRequest,
    ) {
        spot.move(actor, request)
    }

    @ZLinkSpotActorSend
    suspend fun deliver(
        spot: Region,
        actor: Player,
        dispatch: ZLinkMessageContext,
        view: RegionView,
    ) {
        actor.context.boundSession().kotlin().send(view).await()
    }

    @ZLinkSpotActorSend
    suspend fun dungeon(
        spot: Region,
        actor: Player,
        dispatch: ZLinkMessageContext,
        request: OpenDungeonRequest,
    ) {
        require(request.dungeonId.isNotBlank()) { "Dungeon ID is required" }
        val created =
            spots.kotlin().getOrCreate(request.dungeonId, Names.DUNGEON).inMesh(Names.MESH).await()
        check(created.state() != ZLinkSpotCreateState.REJECTED) { "Dungeon creation rejected" }
        val opened =
            spot.context
                .outbound()
                .kotlin()
                .requestToSpot<DungeonOpened>(request.dungeonId, DungeonInfoRequest())
                .await()
        actor.context.boundSession().kotlin().send(opened).await()
    }
}
