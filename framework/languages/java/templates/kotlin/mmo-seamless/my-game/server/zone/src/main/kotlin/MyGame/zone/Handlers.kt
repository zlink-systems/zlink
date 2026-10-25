package MyGame.zone

import MyGame.*
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.handlers.*
import systems.zlink.framework.kotlin.*
import systems.zlink.framework.spots.*

@ZLinkHandlerGroup(Names.PLAYER)
class Actions(private val spots: ZLinkSpotManager) {
    @ZLinkSpotActorSend
    suspend fun reconnect(
        spot: Zone,
        actor: Player,
        dispatch: ZLinkMessageContext,
        request: JoinWorldRequest,
    ) {
        require(request.playerId == actor.context.actorId())
        spot.reconnect(actor)
    }

    @ZLinkSpotActorSend
    suspend fun move(
        spot: Zone,
        actor: Player,
        dispatch: ZLinkMessageContext,
        request: MoveRequest,
    ) {
        spot.move(actor, request)
    }

    @ZLinkSpotActorSend
    suspend fun deliverAppeared(
        spot: Zone,
        actor: Player,
        dispatch: ZLinkMessageContext,
        event: PlayerAppeared,
    ) {
        actor.context.boundSession().kotlin().send(event).await()
    }

    @ZLinkSpotActorSend
    suspend fun deliverMoved(
        spot: Zone,
        actor: Player,
        dispatch: ZLinkMessageContext,
        event: PlayerMoved,
    ) {
        actor.context.boundSession().kotlin().send(event).await()
    }

    @ZLinkSpotActorSend
    suspend fun deliverLeft(
        spot: Zone,
        actor: Player,
        dispatch: ZLinkMessageContext,
        event: PlayerLeft,
    ) {
        actor.context.boundSession().kotlin().send(event).await()
    }

    @ZLinkSpotActorSend
    suspend fun dungeon(
        spot: Zone,
        actor: Player,
        dispatch: ZLinkMessageContext,
        request: OpenDungeonRequest,
    ) {
        val created =
            spots.kotlin().getOrCreate(request.dungeonId, Names.DUNGEON).inMesh(Names.MESH).await()
        check(created.state() != ZLinkSpotCreateState.REJECTED) { "Dungeon creation rejected" }
        val opened =
            spot.context
                .outbound()
                .kotlin()
                .requestToSpot<DungeonOpened>(request.dungeonId, request)
                .await()
        actor.context.boundSession().kotlin().send(opened).await()
    }
}
