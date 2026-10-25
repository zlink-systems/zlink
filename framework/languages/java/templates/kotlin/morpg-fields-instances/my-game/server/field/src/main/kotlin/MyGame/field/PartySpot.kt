package MyGame.field

import MyGame.*
import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionStage
import systems.zlink.framework.actors.*
import systems.zlink.framework.handlers.ZLinkSpotRequest
import systems.zlink.framework.kotlin.*
import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.framework.spots.*

// The party owns membership and run state; its ID is also this Spot ID.
class PartySpot(
    override val context: ZLinkSpotContext,
    private val actors: ZLinkActorClient,
    private val spots: ZLinkSpotManager,
) : ZLinkSuspendingSpot<Player>() {
    var state = PartyRun()

    override fun configure() {}

    override suspend fun onActorJoinSuspending(
        actorId: String,
        request: ZLinkMessage,
    ): ZLinkSpotActorJoinResult = ZLinkSpotActorJoinResult.reject()

    override suspend fun onJoinedActorSuspending(actor: Player) {}

    override suspend fun onLeaveActorSuspending(actor: Player) {}

    suspend fun command(command: PartyCommand): PartyCommandResult {
        val rejection = when {
            command.join -> state.join(command.actorId)
            command.enter -> state.enter(command.actorId)
            command.actorId in state.members -> null
            else -> ActionRejected(Rejection.INVALID_PARTY)
        }
        if (rejection != null) return PartyCommandResult(null, rejection)

        if (command.enter && state.phase != RunPhase.Active) {
            val runId = requireNotNull(state.runId)
            val dungeonId = Rules.dungeonId(runId)
            val created = spots.kotlin().getOrCreate(dungeonId, Names.DUNGEON).inMesh(Names.MESH).await()
            check(created.state() != ZLinkSpotCreateState.REJECTED) { "Dungeon creation rejected" }
            val prepared =
                context.outbound().kotlin().requestToSpot<DungeonPrepared>(
                    dungeonId,
                    PrepareDungeon(runId, context.spotId(), state.members.toList()),
                ).await()
            if (prepared.rejection != null) return PartyCommandResult(null, prepared.rejection)
            check(prepared.dungeonId == dungeonId) { "Dungeon identity mismatch" }
            state.prepared(prepared.runId)?.let { return PartyCommandResult(null, it) }
            for (id in state.members) {
                if (id !in state.entered)
                    actors.kotlin().sendToActor(id, JoinDungeon(dungeonId, runId)).await()
            }
        }
        return finishCommand(command)
    }

    suspend fun progress(runId: String, actorId: String?, completed: Boolean = false): PartyCommandResult {
        val rejection = when {
            actorId != null -> state.arrived(runId, actorId)
            completed -> state.completed(runId)
            else -> state.started(runId)
        }
        if (rejection != null) return PartyCommandResult(null, rejection)
        broadcast()
        return PartyCommandResult(state.view(context.spotId()), null)
    }

    private suspend fun finishCommand(command: PartyCommand): PartyCommandResult {
        if (command.join || command.enter) broadcast()
        return PartyCommandResult(state.view(context.spotId()), null)
    }

    private suspend fun broadcast() {
        val view = state.view(context.spotId())
        for (id in state.members) actors.kotlin().sendToActor(id, view).await()
    }

    fun capture(): ByteArray = Json.mapper.writeValueAsBytes(state)

    fun restore(bytes: ByteArray) {
        state = Json.mapper.readValue(bytes, PartyRun::class.java)
    }
}

class PartyRequests {
    @ZLinkSpotRequest
    suspend fun command(spot: PartySpot, request: PartyCommand) = spot.command(request)

    @ZLinkSpotRequest
    suspend fun memberEntered(spot: PartySpot, request: DungeonMemberEntered) =
        spot.progress(request.runId, request.actorId)

    @ZLinkSpotRequest
    suspend fun started(spot: PartySpot, request: RunStarted) = spot.progress(request.runId, null)

    @ZLinkSpotRequest
    suspend fun completed(spot: PartySpot, request: RunCompleted) =
        spot.progress(request.runId, null, true)
}

class PartyTransfer : ZLinkSpotRelocationAdapter<PartySpot> {
    override fun capture(
        spot: PartySpot,
        cancellation: ZLinkRelocationCancellation,
    ): CompletionStage<ByteArray> {
        cancellation.ensureActive()
        return CompletableFuture.completedFuture(spot.capture())
    }

    override fun restore(
        spot: PartySpot,
        state: ByteArray,
        cancellation: ZLinkRelocationCancellation,
    ): CompletionStage<Void> {
        cancellation.ensureActive()
        spot.restore(state)
        return CompletableFuture.completedFuture(null)
    }
}
