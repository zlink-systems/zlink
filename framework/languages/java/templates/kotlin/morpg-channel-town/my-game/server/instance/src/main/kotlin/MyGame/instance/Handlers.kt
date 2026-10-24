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
        spot.show(actor)
    }

    @ZLinkSpotActorSend
    suspend fun view(
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
    ) = spot.advance()

    @ZLinkSpotActorSend
    suspend fun push(
        spot: Dungeon,
        actor: Player,
        dispatch: ZLinkMessageContext,
        message: DungeonView,
    ) {
        actor.deliver(message.dungeonId, message)
    }

    @ZLinkSpotActorSend
    suspend fun departed(
        spot: Dungeon,
        actor: Player,
        dispatch: ZLinkMessageContext,
        message: TownView,
    ) = actor.deliver(message.townId, message)
}
