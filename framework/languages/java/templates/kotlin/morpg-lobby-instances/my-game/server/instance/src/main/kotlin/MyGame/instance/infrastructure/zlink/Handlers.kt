package MyGame.instance.infrastructure.zlink

import MyGame.*
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.handlers.*
import systems.zlink.framework.kotlin.*

@ZLinkHandlerGroup(Names.PLAYER)
class DungeonActions {
    @ZLinkSpotActorSend
    suspend fun login(
        spot: Dungeon,
        actor: Player,
        context: ZLinkMessageContext,
        request: JoinLobbyRequest,
    ) {
        actor.checkIdentity(request.playerId)
        spot.show(actor)
    }

    @ZLinkSpotActorSend
    suspend fun view(
        spot: Dungeon,
        actor: Player,
        context: ZLinkMessageContext,
        request: ViewRequest,
    ) = spot.show(actor)

    @ZLinkSpotActorSend
    suspend fun advance(
        spot: Dungeon,
        actor: Player,
        context: ZLinkMessageContext,
        request: AdvanceRequest,
    ) = spot.advance()

    @ZLinkSpotActorSend
    suspend fun deliver(
        spot: Dungeon,
        actor: Player,
        context: ZLinkMessageContext,
        request: DungeonView,
    ) {
        actor.deliver(request.dungeonId, request)
    }

    @ZLinkSpotActorSend
    suspend fun departedPartyView(
        spot: Dungeon,
        actor: Player,
        context: ZLinkMessageContext,
        message: PartyView,
    ) = actor.deliver(Names.PARTY_PREFIX + message.partyId, message)
}

@ZLinkHandlerGroup(Names.DUNGEON)
class Prepare {
    @ZLinkSpotRequest
    suspend fun prepare(spot: Dungeon, request: PrepareDungeon): DungeonPrepared =
        spot.prepare(request)
}
