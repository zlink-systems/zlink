package MyGame.instance

import MyGame.*
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.handlers.*
import systems.zlink.framework.kotlin.*

class Prepare {
    @ZLinkSpotRequest
    suspend fun prepare(spot: Dungeon, request: PrepareDungeon) = spot.prepare(request)
}

@ZLinkHandlerGroup(Names.PLAYER)
class DungeonActions {
    @ZLinkSpotActorSend
    suspend fun reconnect(
        spot: Dungeon,
        actor: Player,
        dispatch: ZLinkMessageContext,
        request: JoinWorldRequest,
    ) {
        actor.checkIdentity(request.playerId)
        spot.show(actor)
    }

    @ZLinkSpotActorSend
    suspend fun query(
        spot: Dungeon,
        actor: Player,
        dispatch: ZLinkMessageContext,
        request: ViewRequest,
    ) = spot.show(actor)

    @ZLinkSpotActorSend
    suspend fun advance(
        spot: Dungeon,
        actor: Player,
        dispatch: ZLinkMessageContext,
        request: AdvanceRequest,
    ) = spot.advance(actor)

    @ZLinkSpotActorSend
    suspend fun deliver(
        spot: Dungeon,
        actor: Player,
        dispatch: ZLinkMessageContext,
        message: DungeonView,
    ) {
        actor.deliver(message.dungeonId, message, true)
    }

    @ZLinkSpotActorSend
    suspend fun departedBorderVisible(
        spot: Dungeon,
        actor: Player,
        dispatch: ZLinkMessageContext,
        message: BorderVisible,
    ) = actor.deliver(message.observerFieldId, message, true)

    @ZLinkSpotActorSend
    suspend fun departedFieldView(
        spot: Dungeon,
        actor: Player,
        dispatch: ZLinkMessageContext,
        message: FieldView,
    ) = actor.deliver(message.fieldId, message, true)
}
