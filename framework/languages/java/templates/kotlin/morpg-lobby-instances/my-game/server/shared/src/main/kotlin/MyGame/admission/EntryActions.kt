package MyGame.admission

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
    ) = spot.join(actor, Names.newPartyId(), true)

    @ZLinkSpotActorSend
    suspend fun join(
        spot: Entry,
        actor: Player,
        context: ZLinkMessageContext,
        request: JoinPartyRequest,
    ) = spot.join(actor, request.partyId, false)
}
