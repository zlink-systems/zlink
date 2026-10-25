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
        if (request.channel > GameRules.MAX_INTEGER) {
            actor.context
                .boundSession()
                .kotlin()
                .send(ActionRejected(Rejection.INTEGER_OUT_OF_RANGE, 0))
                .await()
            return
        }
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
        val rejection = actor.earn()
        if (rejection != null) {
            actor.context.boundSession().kotlin().send(rejection).await()
        } else {
            spot.show(actor)
        }
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
    suspend fun channelView(
        spot: Zone,
        actor: Player,
        context: ZLinkMessageContext,
        request: ChannelViewRequest,
    ) {
        spot.showChannelView(actor)
    }

    @ZLinkSpotActorSend
    suspend fun forward(
        spot: Zone,
        actor: Player,
        context: ZLinkMessageContext,
        message: PlayerView,
    ) {
        actor.context.boundSession().kotlin().send(message).await()
    }

    @ZLinkSpotActorSend
    suspend fun forwardChannelView(
        spot: Zone,
        actor: Player,
        context: ZLinkMessageContext,
        message: ChannelView,
    ) {
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
