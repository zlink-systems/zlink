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
        require(request.sequence == actor.position.sequence + 1) {
            "Move sequence must increase by one"
        }
        val destination = EnterField(request.x, request.sequence)
        if (Rules.fieldId(request.x) != spot.context.spotId()) {
            actor.move(destination)
            return
        }
        actor.apply(destination)
        spot.remember(actor)
        actor.sendState()
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
    ) = spot.party(actor, request.partyId, true)

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
    suspend fun deliverField(
        spot: Field,
        actor: Player,
        dispatch: ZLinkMessageContext,
        message: FieldView,
    ) {
        actor.context.boundSession().kotlin().send(message).await()
    }
}

class BorderTick : ZLinkSuspendingSpotTimerHandler<Field> {
    override suspend fun handle(spot: Field, tick: ZLinkTimerTick) = spot.publishBorder()
}
