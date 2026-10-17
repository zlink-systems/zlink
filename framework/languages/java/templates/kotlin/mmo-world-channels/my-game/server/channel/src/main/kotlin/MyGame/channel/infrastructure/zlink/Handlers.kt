package MyGame.channel.infrastructure.zlink

import MyGame.*
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.handlers.*
import systems.zlink.framework.kotlin.*

@ZLinkHandlerGroup(Names.PLAYER)
class Actions {
    @ZLinkSpotActorSend
    suspend fun enter(
        spot: Zone,
        actor: Player,
        context: ZLinkMessageContext,
        request: EnterWorldRequest,
    ) {
        actor.checkIdentity(request.playerId)
        spot.show(actor)
    }

    @ZLinkSpotActorSend
    suspend fun change(
        spot: Zone,
        actor: Player,
        context: ZLinkMessageContext,
        request: ChangeChannelRequest,
    ) {
        actor.context
            .joinSpot(Names.zone(request.channel), EnterWorldRequest(actor.context.actorId()))
            .defer()
    }

    @ZLinkSpotActorSend
    suspend fun earn(
        spot: Zone,
        actor: Player,
        context: ZLinkMessageContext,
        request: EarnRequest,
    ) {
        actor.earn(request.amount)
        spot.show(actor)
    }

    @ZLinkSpotActorSend
    suspend fun view(
        spot: Zone,
        actor: Player,
        context: ZLinkMessageContext,
        request: ViewRequest,
    ) {
        spot.show(actor)
    }

    @ZLinkSpotActorSend
    suspend fun forward(
        spot: Zone,
        actor: Player,
        context: ZLinkMessageContext,
        message: PlayerView,
    ) {
        if (actor.context.spotId().orElse(null) != Names.zone(message.channel)) return
        actor.context.boundSession().kotlin().send(message).await()
    }
}

@ZLinkHandlerGroup(Names.PLAYER)
class EntryActions {
    @ZLinkSpotActorSend
    suspend fun enter(
        spot: Entry,
        actor: Player,
        context: ZLinkMessageContext,
        request: EnterWorldRequest,
    ) {
        actor.checkIdentity(request.playerId)
        actor.context.joinSpot(Names.zone(Names.FIRST_CHANNEL), request).defer()
    }
}
