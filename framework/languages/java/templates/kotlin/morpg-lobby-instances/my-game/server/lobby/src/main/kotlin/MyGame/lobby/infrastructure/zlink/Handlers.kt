package MyGame.lobby.infrastructure.zlink

import MyGame.*
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.handlers.*
import systems.zlink.framework.kotlin.*

@ZLinkHandlerGroup(Names.PLAYER)
class EntryActions {
    @ZLinkSpotActorSend
    suspend fun login(
        spot: Entry,
        actor: Player,
        context: ZLinkMessageContext,
        request: JoinLobbyRequest,
    ) {
        actor.checkIdentity(request.playerId)
        actor.context.boundSession().kotlin().send(LobbyView()).await()
    }

    @ZLinkSpotActorSend
    suspend fun create(
        spot: Entry,
        actor: Player,
        context: ZLinkMessageContext,
        request: CreatePartyRequest,
    ) = spot.join(actor, request.partyId, true)

    @ZLinkSpotActorSend
    suspend fun join(
        spot: Entry,
        actor: Player,
        context: ZLinkMessageContext,
        request: JoinPartyRequest,
    ) = spot.join(actor, request.partyId, false)
}

@ZLinkHandlerGroup(Names.PLAYER)
class PartyActions {
    @ZLinkSpotActorSend
    suspend fun login(
        spot: Party,
        actor: Player,
        context: ZLinkMessageContext,
        request: JoinLobbyRequest,
    ) {
        actor.checkIdentity(request.playerId)
        spot.show(actor)
    }

    @ZLinkSpotActorSend
    suspend fun view(
        spot: Party,
        actor: Player,
        context: ZLinkMessageContext,
        request: ViewRequest,
    ) = spot.show(actor)

    @ZLinkSpotActorSend
    suspend fun ready(
        spot: Party,
        actor: Player,
        context: ZLinkMessageContext,
        request: ReadyRequest,
    ) = spot.ready(actor)

    @ZLinkSpotActorSend
    suspend fun enter(
        spot: Party,
        actor: Player,
        context: ZLinkMessageContext,
        request: EnterDungeonRequest,
    ) = spot.enter(actor)

    @ZLinkSpotActorSend
    suspend fun travel(
        spot: Party,
        actor: Player,
        context: ZLinkMessageContext,
        request: JoinDungeon,
    ) {
        actor.context.joinSpot(request.dungeonId, request).defer()
    }

    @ZLinkSpotActorSend
    suspend fun deliver(
        spot: Party,
        actor: Player,
        context: ZLinkMessageContext,
        request: PartyView,
    ) {
        actor.context.boundSession().kotlin().send(request).await()
    }
}
