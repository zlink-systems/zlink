package MyGame.lobby.infrastructure.zlink

import MyGame.*
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.handlers.*
import systems.zlink.framework.kotlin.*

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
    @ZLinkSpotActorSend
    suspend fun reject(spot: Party, actor: Player, context: ZLinkMessageContext, request: ActionRejected) {
        actor.context.boundSession().kotlin().send(request).await()
    }

}

class Started : ZLinkSuspendingSpotPacketHandler<Party, RunStarted> {
    override suspend fun handle(spot: Party, message: RunStarted) { require(spot.state.started(message.runId) == null) { Rules.RUN_MISMATCH } }
}
class Completed : ZLinkSuspendingSpotPacketHandler<Party, RunCompleted> {
    override suspend fun handle(spot: Party, message: RunCompleted) { require(spot.state.completed(message.runId) == null) { Rules.RUN_MISMATCH } }
}
class EnterRun : ZLinkSuspendingSpotPacketHandler<Party, EnterPartyRun> {
    override suspend fun handle(spot: Party, message: EnterPartyRun) { spot.enterPlayer(message.playerId) }
}

class Arrived : ZLinkSuspendingSpotPacketHandler<Party, DungeonMemberEntered> {
    override suspend fun handle(spot: Party, message: DungeonMemberEntered) { require(spot.state.arrived(message.runId, message.playerId) == null) { Rules.RUN_MISMATCH } }
}
