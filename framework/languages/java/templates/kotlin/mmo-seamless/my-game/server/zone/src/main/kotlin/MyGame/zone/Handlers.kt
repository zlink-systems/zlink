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
        spot.remember(actor)
        actor.sendState()
    }

    @ZLinkSpotActorSend
    suspend fun move(
        spot: Zone,
        actor: Player,
        dispatch: ZLinkMessageContext,
        request: MoveRequest,
    ) {
        require(request.sequence == actor.position.sequence + 1) {
            "Move sequence must increase by one"
        }
        val destination = EnterZone(request.x, request.sequence)
        if (Rules.zoneId(request.x) != spot.context.spotId()) {
            actor.move(destination)
            return
        }
        actor.apply(destination)
        spot.remember(actor)
        actor.sendState()
    }

    @ZLinkSpotActorSend
    suspend fun deliver(
        spot: Zone,
        actor: Player,
        dispatch: ZLinkMessageContext,
        message: BorderVisible,
    ) {
        actor.context.boundSession().kotlin().send(message).await()
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

class BorderTick : ZLinkSuspendingSpotTimerHandler<Zone> {
    override suspend fun handle(spot: Zone, tick: ZLinkTimerTick) = spot.publishBorder()
}
