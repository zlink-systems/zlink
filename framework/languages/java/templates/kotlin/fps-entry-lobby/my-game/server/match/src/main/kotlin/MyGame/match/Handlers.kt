package MyGame.match

import MyGame.*
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.handlers.*
import systems.zlink.framework.kotlin.*

@ZLinkHandlerGroup(Names.PLAYER)
class Actions {
    @ZLinkSpotActorSend
    suspend fun queue(
        spot: Matchmaking,
        actor: Player,
        dispatch: ZLinkMessageContext,
        request: QueueRequest,
    ) = spot.queue(actor, request)

    @ZLinkSpotActorSend
    suspend fun assign(
        spot: Matchmaking,
        actor: Player,
        dispatch: ZLinkMessageContext,
        message: RoomAssignment,
    ) {
        actor.context
            .joinSpot(message.roomId, RoomAdmission(actor.context.actorId(), message.roomId))
            .defer()
    }

    @ZLinkSpotActorSend
    suspend fun reconnect(
        spot: Room,
        actor: Player,
        dispatch: ZLinkMessageContext,
        request: QueueRequest,
    ) {
        require(request.playerId == actor.context.actorId()) { "Reconnect identity mismatch" }
        spot.sendView(actor)
    }

    @ZLinkSpotActorSend
    suspend fun view(
        spot: Room,
        actor: Player,
        dispatch: ZLinkMessageContext,
        request: RoomViewRequest,
    ) = spot.sendView(actor)

    @ZLinkSpotActorSend
    suspend fun move(
        spot: Room,
        actor: Player,
        dispatch: ZLinkMessageContext,
        request: MoveRequest,
    ) = spot.move(actor, request)

    @ZLinkSpotActorSend
    suspend fun shoot(
        spot: Room,
        actor: Player,
        dispatch: ZLinkMessageContext,
        request: ShootRequest,
    ) = spot.shoot(actor, request)

    @ZLinkSpotActorSend
    suspend fun viewPush(
        spot: Room,
        actor: Player,
        dispatch: ZLinkMessageContext,
        message: RoomView,
    ) {
        actor.context.boundSession().kotlin().send(message).await()
    }

    @ZLinkSpotActorSend
    suspend fun foundPush(
        spot: Room,
        actor: Player,
        dispatch: ZLinkMessageContext,
        message: MatchFound,
    ) {
        actor.context.boundSession().kotlin().send(message).await()
    }

    @ZLinkSpotActorSend
    suspend fun endedPush(
        spot: Room,
        actor: Player,
        dispatch: ZLinkMessageContext,
        message: MatchEnded,
    ) {
        actor.context.boundSession().kotlin().send(message).await()
    }
}
