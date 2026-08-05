package systems.zlink.samples.kotlin.tictactoe.server.play.infrastructure.zlink.actors

import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionStage
import systems.zlink.framework.actors.ZLinkActor
import systems.zlink.framework.actors.ZLinkActorContext
import systems.zlink.framework.actors.ZLinkActorJoinCompletion
import systems.zlink.framework.actors.ZLinkActorJoinOperationId
import systems.zlink.samples.kotlin.tictactoe.shared.contracts.JoinGameFailedNotify
import systems.zlink.samples.kotlin.tictactoe.shared.contracts.JoinGameRes
import systems.zlink.samples.kotlin.tictactoe.shared.contracts.PlayerInfo
import systems.zlink.samples.kotlin.tictactoe.shared.contracts.TicTacToeGameJoinRes

class PlayActor(
    val actorId: String,
    private val context: ZLinkActorContext,
) : ZLinkActor {
    private var joinedRoomId: String? = null
    private var pendingRoomId: String? = null
    private val completedJoinOperations = mutableSetOf<ZLinkActorJoinOperationId>()
    private var player: PlayerInfo? = null
    var destroyAfterEntrySpotJoin: Boolean = false
        private set
    var disconnected: Boolean = false
        private set

    override fun context(): ZLinkActorContext = context

    fun applyPlayer(player: PlayerInfo) {
        require(player.actorId == actorId) { "player actor id does not match actor" }
        this.player = player
    }

    fun requirePlayer(): PlayerInfo =
        player ?: throw IllegalStateException("actor has not been authenticated")

    fun playerOrNull(): PlayerInfo? = player

    fun incrementWins(): Int {
        val current = requirePlayer()
        val updated = current.copy(wins = current.wins + 1)
        player = updated
        return updated.wins
    }

    fun joinGame(roomId: String) {
        joinedRoomId = roomId
    }

    fun trackDeferredJoin(roomId: String) {
        check(pendingRoomId == null) { "a room join is already pending" }
        pendingRoomId = roomId
    }

    override fun onJoinCompleted(completion: ZLinkActorJoinCompletion): CompletionStage<Void> {
        val operationId = when (completion) {
            is ZLinkActorJoinCompletion.Accepted -> completion.operationId()
            is ZLinkActorJoinCompletion.Rejected -> completion.operationId()
            is ZLinkActorJoinCompletion.Failed -> completion.operationId()
        }
        if (!completedJoinOperations.add(operationId)) {
            return CompletableFuture.completedFuture(null)
        }

        var roomId = pendingRoomId
        pendingRoomId = null
        return when (completion) {
            is ZLinkActorJoinCompletion.Accepted -> {
                val reply = completion.reply().decode(TicTacToeGameJoinRes::class.java)
                if (roomId.isNullOrBlank()) {
                    // The relocated Actor reconstructs the room from the durable accepted reply.
                    roomId = reply.state.roomId
                }
                joinGame(roomId.orEmpty())
                context.boundSession().send(JoinGameRes(reply.state)).submit()
            }
            is ZLinkActorJoinCompletion.Rejected ->
                context.boundSession()
                    .send(JoinGameFailedNotify(roomId.orEmpty(), "Rejected", false))
                    .submit()
            is ZLinkActorJoinCompletion.Failed ->
                context.boundSession()
                    .send(
                        JoinGameFailedNotify(
                            roomId.orEmpty(),
                            completion.kind().name,
                            false,
                        ),
                    )
                    .submit()
        }
    }

    fun requireJoinedGame(): String =
        joinedRoomId ?: throw IllegalStateException("actor has not joined a game")

    fun joinedRoomIdOrNull(): String? = joinedRoomId

    fun markForDestroyAfterRoomLeave() {
        destroyAfterEntrySpotJoin = true
    }

    fun markDisconnected() {
        disconnected = true
    }
}
