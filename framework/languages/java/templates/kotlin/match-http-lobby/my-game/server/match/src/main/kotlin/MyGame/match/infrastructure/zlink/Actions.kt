package MyGame.match.infrastructure.zlink

import MyGame.*
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.handlers.*

@ZLinkHandlerGroup(Names.PLAYER)
class Actions {
    @ZLinkSpotActorSend
    suspend fun join(
        room: Room,
        actor: Player,
        dispatch: ZLinkMessageContext,
        request: JoinRoomRequest,
    ) {
        room.checkJoin(actor.context.actorId(), request)
        room.current(actor)
    }

    @ZLinkSpotActorSend
    suspend fun view(
        room: Room,
        actor: Player,
        dispatch: ZLinkMessageContext,
        request: RoomViewRequest,
    ) {
        room.current(actor)
    }

    @ZLinkSpotActorSend
    suspend fun move(
        room: Room,
        actor: Player,
        dispatch: ZLinkMessageContext,
        request: MoveRequest,
    ) {
        room.move(actor, request)
    }

    @ZLinkSpotActorSend
    suspend fun shoot(
        room: Room,
        actor: Player,
        dispatch: ZLinkMessageContext,
        request: ShootRequest,
    ) {
        room.shoot(actor, request)
    }

    @ZLinkSpotActorSend
    suspend fun forward(
        room: Room,
        actor: Player,
        dispatch: ZLinkMessageContext,
        message: RoomView,
    ) {
        Room.reply(actor, message)
    }

    @ZLinkSpotActorSend
    suspend fun forward(
        room: Room,
        actor: Player,
        dispatch: ZLinkMessageContext,
        message: MatchEnded,
    ) {
        Room.reply(actor, message)
    }
}
