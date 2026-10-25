package MyGame.town

import MyGame.*
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.handlers.*
import systems.zlink.framework.kotlin.*

@ZLinkHandlerGroup(Names.PLAYER)
class Actions {
    @ZLinkSpotActorSend
    suspend fun reconnect(
        spot: Town,
        actor: Player,
        dispatch: ZLinkMessageContext,
        request: JoinTownRequest,
    ) {
        require(request.playerId == actor.context.actorId()) { "Player identity mismatch" }
        spot.show(actor)
    }

    @ZLinkSpotActorSend
    suspend fun change(
        spot: Town,
        actor: Player,
        dispatch: ZLinkMessageContext,
        request: ChangeChannelRequest,
    ) {
        spot.changeChannel(actor, request.channel)
    }

    @ZLinkSpotActorSend
    suspend fun create(
        spot: Town,
        actor: Player,
        dispatch: ZLinkMessageContext,
        request: CreatePartyRequest,
    ) = spot.party(actor, null, true)

    @ZLinkSpotActorSend
    suspend fun joinParty(
        spot: Town,
        actor: Player,
        dispatch: ZLinkMessageContext,
        request: JoinPartyRequest,
    ) = spot.party(actor, request.partyId, false)

    @ZLinkSpotActorSend
    suspend fun enter(
        spot: Town,
        actor: Player,
        dispatch: ZLinkMessageContext,
        request: EnterDungeonRequest,
    ) = spot.enter(actor)

    @ZLinkSpotActorSend
    suspend fun travel(
        spot: Town,
        actor: Player,
        dispatch: ZLinkMessageContext,
        message: JoinDungeon,
    ) {
        actor.context.joinSpot(message.dungeonId, message).defer()
    }

    @ZLinkSpotActorSend
    suspend fun view(
        spot: Town,
        actor: Player,
        dispatch: ZLinkMessageContext,
        request: ViewRequest,
    ) = spot.show(actor)

    @ZLinkSpotActorSend
    suspend fun push(spot: Town, actor: Player, dispatch: ZLinkMessageContext, message: TownView) {
        actor.context.boundSession().kotlin().send(message).await()
    }
}

@ZLinkHandlerGroup(Names.TOWN)
class RunStartedHandler : ZLinkSuspendingSpotPacketHandler<Town, RunStarted> {
    override suspend fun handle(spot: Town, message: RunStarted) = spot.runStarted(message)
}

@ZLinkHandlerGroup(Names.TOWN)
class RunCompletedHandler : ZLinkSuspendingSpotPacketHandler<Town, RunCompleted> {
    override suspend fun handle(spot: Town, message: RunCompleted) = spot.runCompleted(message)
}

@ZLinkHandlerGroup(Names.TOWN)
class PartyViewRequestHandler : ZLinkSuspendingSpotPacketHandler<Town, PartyViewRequest> {
    override suspend fun handle(spot: Town, message: PartyViewRequest) =
        spot.showParty(message.playerId)
}
