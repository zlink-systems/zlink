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
        actor.context.boundSession().kotlin().send(message).await()
    }

    @ZLinkSpotActorSend
    suspend fun deliverParty(
        spot: Dungeon,
        actor: Player,
        dispatch: ZLinkMessageContext,
        message: PartyView,
    ) {
        actor.context.boundSession().kotlin().send(message).await()
    }

    @ZLinkSpotActorSend
    suspend fun enter(
        spot: Dungeon,
        actor: Player,
        dispatch: ZLinkMessageContext,
        request: EnterDungeonRequest,
    ) = spot.enter(actor)

    @ZLinkSpotActorSend
    suspend fun createParty(
        spot: Dungeon,
        actor: Player,
        dispatch: ZLinkMessageContext,
        request: CreatePartyRequest,
    ) = spot.rejectTownRequest(actor)

    @ZLinkSpotActorSend
    suspend fun joinParty(
        spot: Dungeon,
        actor: Player,
        dispatch: ZLinkMessageContext,
        request: JoinPartyRequest,
    ) = spot.rejectTownRequest(actor)

    @ZLinkSpotActorSend
    suspend fun travel(
        spot: Dungeon,
        actor: Player,
        dispatch: ZLinkMessageContext,
        request: JoinDungeon,
    ) {
        if (request.dungeonId != spot.context.spotId())
            actor.context.joinSpot(request.dungeonId, request).defer()
    }
}
