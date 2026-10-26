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

    @ZLinkSpotActorSend
    suspend fun notificationPartyView(spot: Entry, actor: Player, context: ZLinkMessageContext, message: PartyView) {
        actor.deliver(message)
    }

    @ZLinkSpotActorSend
    suspend fun notificationDungeonView(spot: Entry, actor: Player, context: ZLinkMessageContext, message: DungeonView) {
        actor.deliver(message)
    }

    @ZLinkSpotActorSend
    suspend fun notificationActionRejected(spot: Entry, actor: Player, context: ZLinkMessageContext, message: ActionRejected) {
        actor.deliver(message)
    }
}
