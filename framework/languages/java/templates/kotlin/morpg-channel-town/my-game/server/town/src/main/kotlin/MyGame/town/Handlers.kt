package MyGame.town
import MyGame.*
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.handlers.*
import systems.zlink.framework.kotlin.*
@ZLinkHandlerGroup(Names.PLAYER)
class Actions {
    @ZLinkSpotActorSend
    suspend fun join(entry: Entry, actor: Player, dispatch: ZLinkMessageContext, request: JoinTownRequest) {
        require(request.playerId == actor.context.actorId()) { "Player identity mismatch" }
        actor.context.joinSpot(Names.townId(request.channel), request).defer()
    }
    @ZLinkSpotActorSend
    suspend fun reconnect(spot: Town, actor: Player, dispatch: ZLinkMessageContext, request: JoinTownRequest) {
        require(request.playerId == actor.context.actorId()) { "Player identity mismatch" }
        spot.show(actor)
    }
    @ZLinkSpotActorSend
    suspend fun change(spot: Town, actor: Player, dispatch: ZLinkMessageContext, request: ChangeChannelRequest) {
        actor.context.joinSpot(Names.townId(request.channel), JoinTownRequest(actor.context.actorId(), request.channel)).defer()
    }
    @ZLinkSpotActorSend
    suspend fun create(spot: Town, actor: Player, dispatch: ZLinkMessageContext, request: CreatePartyRequest) = spot.party(actor, request.partyId, true)
    @ZLinkSpotActorSend
    suspend fun joinParty(spot: Town, actor: Player, dispatch: ZLinkMessageContext, request: JoinPartyRequest) = spot.party(actor, request.partyId, false)
    @ZLinkSpotActorSend
    suspend fun enter(spot: Town, actor: Player, dispatch: ZLinkMessageContext, request: EnterDungeonRequest) = spot.enter(actor)
    @ZLinkSpotActorSend
    suspend fun travel(spot: Town, actor: Player, dispatch: ZLinkMessageContext, message: JoinDungeon) {
        actor.context.joinSpot(message.dungeonId, message).defer()
    }
    @ZLinkSpotActorSend
    suspend fun view(spot: Town, actor: Player, dispatch: ZLinkMessageContext, request: ViewRequest) = spot.show(actor)
    @ZLinkSpotActorSend
    suspend fun push(spot: Town, actor: Player, dispatch: ZLinkMessageContext, message: TownView) {
        actor.context.boundSession().kotlin().send(message).await()
    }
}
