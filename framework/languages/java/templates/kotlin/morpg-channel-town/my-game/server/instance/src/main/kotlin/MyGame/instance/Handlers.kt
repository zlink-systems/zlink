package MyGame.instance

import MyGame.*
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.handlers.*
import systems.zlink.framework.kotlin.*

@ZLinkHandlerGroup(Names.DUNGEON)
class Preparation {
    @ZLinkSpotRequest
    suspend fun prepare(spot: Dungeon, request: PrepareDungeon): DungeonPrepared =
        spot.prepare(request)
}

@ZLinkHandlerGroup(Names.PLAYER)
class Actions {
    @ZLinkSpotActorSend
    suspend fun reconnect(
        spot: Dungeon,
        actor: Player,
        dispatch: ZLinkMessageContext,
        request: JoinTownRequest,
    ) {
        require(request.playerId == actor.context.actorId()) { "Player identity mismatch" }
        spot.showWithParty(actor)
    }

    @ZLinkSpotActorSend
    suspend fun view(
        spot: Dungeon,
        actor: Player,
        dispatch: ZLinkMessageContext,
        request: ViewRequest,
    ) = spot.showWithParty(actor)

    @ZLinkSpotActorSend
    suspend fun advance(
        spot: Dungeon,
        actor: Player,
        dispatch: ZLinkMessageContext,
        request: AdvanceRequest,
    ) = spot.advance(actor)

    @ZLinkSpotActorSend
    suspend fun enterAgain(
        spot: Dungeon,
        actor: Player,
        dispatch: ZLinkMessageContext,
        request: EnterDungeonRequest,
    ) = spot.show(actor)

    @ZLinkSpotActorSend
    suspend fun travelAgain(
        spot: Dungeon,
        actor: Player,
        dispatch: ZLinkMessageContext,
        request: JoinDungeon,
    ) = Unit

    @ZLinkSpotActorSend
    suspend fun changeChannel(
        spot: Dungeon,
        actor: Player,
        dispatch: ZLinkMessageContext,
        request: ChangeChannelRequest,
    ) = actor.context.boundSession().kotlin().send(ActionRejected(Rejection.RUN_IN_PROGRESS)).await()

    @ZLinkSpotActorSend
    suspend fun push(
        spot: Dungeon,
        actor: Player,
        dispatch: ZLinkMessageContext,
        message: DungeonView,
    ) {
        actor.context.boundSession().kotlin().send(message).await()
    }

    @ZLinkSpotActorSend
    suspend fun pushTown(
        spot: Dungeon,
        actor: Player,
        dispatch: ZLinkMessageContext,
        message: TownView,
    ) {
        actor.context.boundSession().kotlin().send(message).await()
    }
}
