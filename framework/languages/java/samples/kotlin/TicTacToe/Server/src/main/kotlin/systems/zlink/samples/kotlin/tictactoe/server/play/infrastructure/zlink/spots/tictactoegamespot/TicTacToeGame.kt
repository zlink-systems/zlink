package systems.zlink.samples.kotlin.tictactoe.server.play.infrastructure.zlink.spots.tictactoegamespot

import com.fasterxml.jackson.databind.ObjectMapper
import java.time.Duration
import java.time.Instant
import kotlinx.coroutines.future.await
import systems.zlink.framework.kotlin.addHandler
import systems.zlink.framework.kotlin.ZLinkSuspendingSpot
import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.framework.spots.ZLinkSpotActorJoinResult
import systems.zlink.framework.spots.ZLinkSpotClosingContext
import systems.zlink.framework.spots.ZLinkSpotContext
import systems.zlink.framework.spots.ZLinkSpotCreateResponse
import systems.zlink.framework.spots.ZLinkTimer
import systems.zlink.samples.kotlin.tictactoe.server.configuration.SampleNames
import systems.zlink.samples.kotlin.tictactoe.server.play.domain.tictactoe.TicTacToeMatch
import systems.zlink.samples.kotlin.tictactoe.server.play.infrastructure.zlink.actors.PlayActor
import systems.zlink.samples.kotlin.tictactoe.server.play.infrastructure.zlink.spots.tictactoegamespot.handlers.PlayActorLeaveGameHandler
import systems.zlink.samples.kotlin.tictactoe.server.play.infrastructure.zlink.spots.tictactoegamespot.handlers.PlayActorPlaceMarkHandler
import systems.zlink.samples.kotlin.tictactoe.server.play.infrastructure.zlink.spots.tictactoegamespot.handlers.TicTacToeGameCreatedHandler
import systems.zlink.samples.kotlin.tictactoe.server.play.infrastructure.zlink.spots.tictactoegamespot.handlers.TicTacToeGameTimerHandler
import systems.zlink.samples.kotlin.tictactoe.shared.contracts.GameState
import systems.zlink.samples.kotlin.tictactoe.shared.contracts.GameStateNotify
import systems.zlink.samples.kotlin.tictactoe.shared.contracts.PlaceMarkRes
import systems.zlink.samples.kotlin.tictactoe.shared.contracts.PlayerInfo
import systems.zlink.samples.kotlin.tictactoe.shared.contracts.PlayerJoinedNotify
import systems.zlink.samples.kotlin.tictactoe.shared.contracts.PlayerWinMilestoneMsg
import systems.zlink.samples.kotlin.tictactoe.shared.contracts.TicTacToeGameJoinReq
import systems.zlink.samples.kotlin.tictactoe.shared.contracts.TicTacToeGameJoinRes

class TicTacToeGame(
    override val context: ZLinkSpotContext,
    private val createdHandler: TicTacToeGameCreatedHandler,
    private val json: ObjectMapper,) : ZLinkSuspendingSpot<PlayActor>() {
    private val gameTickPeriod: Duration = Duration.ofSeconds(1)
    private val turnTimeout: Duration = Duration.ofSeconds(15)
    val roomId: String = context.spotId()
    private val match = TicTacToeMatch(roomId, turnTimeout)
    private val players = mutableListOf<PlayerSlot>()
    private val pendingJoins = mutableMapOf<String, TicTacToeGameJoinReq>()
    private var gameTick: ZLinkTimer? = null
    private var created = false

    override suspend fun onCreateSuspending(request: ZLinkMessage): ZLinkSpotCreateResponse {
        createdHandler.handle(this, request)
        return ZLinkSpotCreateResponse.accept()
    }

    override suspend fun onActorJoinSuspending(
        actorId: String,
        request: ZLinkMessage,
    ): ZLinkSpotActorJoinResult {
        val joinRequest = request.decode(TicTacToeGameJoinReq::class.java)
        require(joinRequest.player.actorId == actorId) {
            "join request actor id does not match bound actor"
        }
        validateJoin(joinRequest.roomId, joinRequest.player)
        val preview = match.previewJoin(actorId)
        pendingJoins[actorId] = joinRequest
        return ZLinkSpotActorJoinResult.accept(TicTacToeGameJoinRes(preview.state))
    }

    override suspend fun onJoinedActorSuspending(actor: PlayActor) {
        val joinRequest = pendingJoins.remove(actor.actorId)
            ?: error("joined actor does not have a pending admission")
        join(actor, joinRequest.roomId, joinRequest.player)
    }

    override suspend fun onLeaveActorSuspending(actor: PlayActor) {
        players.removeIf { it.actor.actorId == actor.actorId }
    }

    override suspend fun onDisconnectActorSuspending(actor: PlayActor) {
        actor.markDisconnected()
    }

    override suspend fun onInitializeSuspending() {
        gameTick = context.addTimer(
            "game-tick",
            gameTickPeriod,
            TicTacToeGameTimerHandler::class.java,
            null,
        ).await()
    }

    override suspend fun onClosingSuspending(context: ZLinkSpotClosingContext) {
        gameTick?.cancel()?.await()
    }

    fun markCreated(request: ZLinkMessage) {
        require(request.isEmpty()) { "tic-tac-toe game creation does not accept payload parts" }
        created = true
    }

    fun join(actor: PlayActor, roomId: String, player: PlayerInfo): TicTacToeGameJoinRes {
        validateJoin(roomId, player)
        actor.applyPlayer(player)
        val change = match.joinPlayer(actor.actorId, Instant.now())
        var slot = players.firstOrNull { it.actor.actorId == actor.actorId }
        if (slot == null) {
            slot = PlayerSlot(actor, change.mark)
            players += slot
        } else {
            slot.actor = actor
        }
        actor.joinGame(roomId)
        val state = change.state
        if (change.isNewPlayer) {
            notifyPlayerJoined(actor, slot, state)
        }
        broadcast(state, actor.actorId)
        return TicTacToeGameJoinRes(state)
    }

    private fun validateJoin(roomId: String, player: PlayerInfo) {
        ensureCreated()
        check(roomId == this.roomId) { "join request room id does not match game room" }
        check(player.level >= SampleNames.RequiredLevel) { "player level does not satisfy room requirement" }
    }

    suspend fun placeMark(actor: PlayActor, cell: Int): PlaceMarkRes {
        ensureCreated()
        val slot = players.firstOrNull { it.actor.actorId == actor.actorId }
            ?: throw IllegalStateException("player has not joined")

        val change = match.placeMark(actor.actorId, cell, Instant.now())
        val state = change.after
        broadcast(state, actor.actorId)
        publishWinMilestone(actor, change.before, state)
        return PlaceMarkRes(state)
    }

    fun hasPlayer(actorId: String): Boolean =
        players.any { it.actor.actorId == actorId }

    private fun snapshot(): GameState {
        ensureCreated()
        return match.snapshot()
    }

    suspend fun tick() {
        ensureCreated()
        val change = match.tick(Instant.now())
        if (!change.changed) {
            return
        }

        val state = change.state
        broadcast(state, null)
    }

    private fun ensureCreated() {
        check(created) { "tic-tac-toe game has not completed creation" }
    }

    private fun broadcast(state: GameState, excludedActorId: String?) {
        players
            .asSequence()
            .map { it.actor }
            .filter { excludedActorId == null || it.actorId != excludedActorId }
            .forEach { actor ->
                actor.context().boundSession()
                    .send(GameStateNotify(state))
                    .submit()
            }
    }

    private fun notifyPlayerJoined(
        joinedActor: PlayActor,
        joinedSlot: PlayerSlot,
        state: GameState,
    ) {
        val player = joinedActor.requirePlayer()
        val message = PlayerJoinedNotify(
            roomId = state.roomId,
            actorId = joinedActor.actorId,
            displayName = player.displayName,
            level = player.level,
            mark = joinedSlot.mark,
            state = state,
        )
        players
            .asSequence()
            .map { it.actor }
            .filter { it.actorId != joinedActor.actorId }
            .forEach { actor ->
                actor.context().boundSession()
                    .send(message)
                    .submit()
            }
    }

    private data class PlayerSlot(var actor: PlayActor, val mark: String)

    suspend fun leaveGame(actor: PlayActor, roomId: String) {
        check(this.roomId == roomId) { "leave request room id does not match game room" }
        if (!isTerminal(match.snapshot())) {
            return
        }
        actor.markForDestroyAfterRoomLeave()
        context.leaveActor(actor).await()
    }

    private fun isTerminal(state: GameState): Boolean =
        state.status == "Won" ||
            state.status == "Draw" ||
            state.status == "TurnTimedOut"

    private suspend fun publishWinMilestone(
        actor: PlayActor,
        before: GameState,
        after: GameState,
    ) {
        if (after.status != "Won" || before.status == "Won" || after.winner != actor.actorId) {
            return
        }
        val player = actor.requirePlayer()
        val wins = actor.incrementWins()
        if (player.wins < 99 || wins != 100) {
            return
        }
        context.outbound()
            .publish(
                SampleNames.PlayNode,
                SampleNames.PlayerMilestoneTopic,
                PlayerWinMilestoneMsg(
                    roomId = after.roomId,
                    actorId = actor.actorId,
                    displayName = player.displayName,
                    wins = wins,
                ),
            )
            .submit()
    }
}
