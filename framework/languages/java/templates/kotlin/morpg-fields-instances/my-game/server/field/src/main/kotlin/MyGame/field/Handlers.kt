package MyGame.field

import MyGame.*
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.handlers.*
import systems.zlink.framework.kotlin.*
import systems.zlink.framework.spots.*

@ZLinkHandlerGroup(Names.PLAYER)
class Actions {
    @ZLinkSpotActorSend
    suspend fun reconnect(
        spot: Field,
        actor: Player,
        dispatch: ZLinkMessageContext,
        request: JoinWorldRequest,
    ) {
        actor.checkIdentity(request.playerId)
        spot.remember(actor)
        actor.sendState()
    }

    @ZLinkSpotActorSend
    suspend fun move(
        spot: Field,
        actor: Player,
        dispatch: ZLinkMessageContext,
        request: MoveRequest,
    ) {
        spot.move(actor, request)
    }

    @ZLinkSpotActorSend
    suspend fun deliver(
        spot: Field,
        actor: Player,
        dispatch: ZLinkMessageContext,
        message: BorderVisible,
    ) {
        actor.context.boundSession().kotlin().send(message).await()
    }

    @ZLinkSpotActorSend
    suspend fun createParty(
        spot: Field,
        actor: Player,
        dispatch: ZLinkMessageContext,
        request: CreatePartyRequest,
    ) = spot.party(actor, null, true)

    @ZLinkSpotActorSend
    suspend fun joinParty(
        spot: Field,
        actor: Player,
        dispatch: ZLinkMessageContext,
        request: JoinPartyRequest,
    ) = spot.party(actor, request.partyId, false)

    @ZLinkSpotActorSend
    suspend fun enterDungeon(
        spot: Field,
        actor: Player,
        dispatch: ZLinkMessageContext,
        request: EnterDungeonRequest,
    ) = spot.enter(actor)

    @ZLinkSpotActorSend
    suspend fun travel(
        spot: Field,
        actor: Player,
        dispatch: ZLinkMessageContext,
        request: JoinDungeon,
    ) {
        actor.context.joinSpot(request.dungeonId, request).defer()
    }

    @ZLinkSpotActorSend
    suspend fun deliverParty(
        spot: Field,
        actor: Player,
        dispatch: ZLinkMessageContext,
        message: PartyView,
    ) {
        actor.context.boundSession().kotlin().send(message).await()
    }

    @ZLinkSpotActorSend
    suspend fun query(
        spot: Field,
        actor: Player,
        dispatch: ZLinkMessageContext,
        request: ViewRequest,
    ) = spot.show(actor)
}

class BorderTick : ZLinkSuspendingSpotTimerHandler<Field> {
    override suspend fun handle(spot: Field, tick: ZLinkTimerTick) = spot.publishBorder()
}
