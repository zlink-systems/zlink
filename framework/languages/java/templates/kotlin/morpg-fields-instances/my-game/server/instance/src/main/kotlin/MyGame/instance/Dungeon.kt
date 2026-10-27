package MyGame.instance

import MyGame.*
import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionStage
import systems.zlink.framework.actors.*
import systems.zlink.framework.kotlin.*
import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.framework.spots.*

// Game instances use User Spots so that Player Actors retain their membership.
class Dungeon(
    override val context: ZLinkSpotContext,
    private val config: NodeConfig,
    private val actors: ZLinkActorClient,
) : ZLinkSuspendingSpot<Player>() {
    var state = DungeonRun()

    suspend fun prepare(request: PrepareDungeon): DungeonPrepared {
        val rejection =
            if (Rules.dungeonId(request.runId) == context.spotId()) state.prepare(request)
            else ActionRejected(Rejection.RUN_MISMATCH)
        return DungeonPrepared(context.spotId(), request.runId, rejection)
    }

    override suspend fun onActorJoinSuspending(
        actorId: String,
        request: ZLinkMessage,
    ): ZLinkSpotActorJoinResult {
        val join = request.decode<JoinDungeon>()
        return if (join.dungeonId == context.spotId() && state.canJoin(actorId, join.runId))
            ZLinkSpotActorJoinResult.accept()
        else ZLinkSpotActorJoinResult.reject()
    }

    override suspend fun onJoinedActorSuspending(actor: Player) {
        val actorId = actor.context.actorId()
        state.arrived(actorId)
        val entered =
            context.outbound().kotlin().requestToSpot<PartyCommandResult>(
                state.partyId,
                DungeonMemberEntered(state.runId, actorId),
            ).await()
        check(entered.rejection == null) { entered.rejection?.reason ?: "Party rejected member arrival" }
        if (state.phase != RunPhase.Active) return

        val started =
            context.outbound().kotlin().requestToSpot<PartyCommandResult>(
                state.partyId,
                RunStarted(state.runId),
            ).await()
        check(started.rejection == null) { started.rejection?.reason ?: "Party rejected run start" }
        sendToMembers(view())
    }

    override suspend fun onLeaveActorSuspending(actor: Player) {}

    override suspend fun onDisconnectActorSuspending(actor: Player) {}

    private fun view() =
        DungeonView(
            context.spotId(),
            config.id,
            state.partyId,
            state.members.toList(),
            state.stage,
            state.runId,
            state.phase,
        )

    suspend fun show(actor: Player) {
        actor.context.boundSession().kotlin().send(view()).await()
    }

    suspend fun advance(requester: Player) {
        val rejection = state.advance()
        if (rejection != null) {
            requester.context.boundSession().kotlin().send(rejection).await()
            return
        }
        if (state.phase == RunPhase.Completed) {
            val completed =
                context.outbound().kotlin().requestToSpot<PartyCommandResult>(
                    state.partyId,
                    RunCompleted(state.runId),
                ).await()
            check(completed.rejection == null) {
                completed.rejection?.reason ?: "Party rejected run completion"
            }
        }
        val current = view()
        requester.context.boundSession().kotlin().send(current).await()
        sendToMembersExcept(current, requester.context.actorId())
    }

    suspend fun enter(actor: Player) = show(actor)

    suspend fun rejectTownRequest(actor: Player) {
        actor.context.boundSession().kotlin().send(ActionRejected(Rejection.RUN_IN_PROGRESS)).await()
    }

    private suspend fun sendToMembers(current: DungeonView) {
        for (id in state.members) actors.kotlin().sendToActor(id, current).await()
    }

    private suspend fun sendToMembersExcept(current: DungeonView, excluded: String) {
        for (id in state.members) {
            if (id != excluded) actors.kotlin().sendToActor(id, current).await()
        }
    }

    fun capture(): ByteArray = Json.mapper.writeValueAsBytes(state)

    fun restore(bytes: ByteArray) {
        state = Json.mapper.readValue(bytes, DungeonRun::class.java)
    }
}

class DungeonTransfer : ZLinkSpotRelocationAdapter<Dungeon> {
    override fun capture(
        spot: Dungeon,
        cancellation: ZLinkRelocationCancellation,
    ): CompletionStage<ByteArray> {
        cancellation.ensureActive()
        return CompletableFuture.completedFuture(spot.capture())
    }

    override fun restore(
        spot: Dungeon,
        state: ByteArray,
        cancellation: ZLinkRelocationCancellation,
    ): CompletionStage<Void> {
        cancellation.ensureActive()
        spot.restore(state)
        return CompletableFuture.completedFuture(null)
    }
}
