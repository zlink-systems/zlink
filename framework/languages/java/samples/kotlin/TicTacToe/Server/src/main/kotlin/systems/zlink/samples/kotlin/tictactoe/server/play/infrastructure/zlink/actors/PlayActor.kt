package systems.zlink.samples.kotlin.tictactoe.server.play.infrastructure.zlink.actors

import systems.zlink.framework.actors.ZLinkActorContext
import systems.zlink.framework.actors.ZLinkActorJoinCompletion
import systems.zlink.framework.actors.ZLinkActorJoinOperationId
import systems.zlink.framework.kotlin.ZLinkSuspendingActor
import systems.zlink.framework.kotlin.decode
import systems.zlink.framework.kotlin.kotlin
import systems.zlink.samples.kotlin.tictactoe.shared.contracts.JoinGameFailedNotify
import systems.zlink.samples.kotlin.tictactoe.shared.contracts.JoinGameNotify
import systems.zlink.samples.kotlin.tictactoe.shared.contracts.PlayerInfo
import systems.zlink.samples.kotlin.tictactoe.shared.contracts.TicTacToeGameJoinRes

class PlayActor(val actorId: String, override val context: ZLinkActorContext) :
    ZLinkSuspendingActor() {
    private var joinedRoomId: String? = null
    private var pendingRoomId: String? = null
    private val completedJoinOperations = mutableSetOf<ZLinkActorJoinOperationId>()
    private var player: PlayerInfo? = null
    var destroyAfterEntrySpotJoin: Boolean = false
        private set

    var disconnected: Boolean = false
        private set

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

    // --8<-- [start:doc-join-completed]
    override suspend fun onJoinCompletedSuspending(completion: ZLinkActorJoinCompletion) {
        val operationId =
            when (completion) {
                is ZLinkActorJoinCompletion.Accepted -> completion.operationId()
                is ZLinkActorJoinCompletion.Rejected -> completion.operationId()
                is ZLinkActorJoinCompletion.Failed -> completion.operationId()
            }
        if (!completedJoinOperations.add(operationId)) {
            return
        }

        var roomId = pendingRoomId
        pendingRoomId = null
        when (completion) {
            is ZLinkActorJoinCompletion.Accepted -> {
                val reply = completion.reply().decode<TicTacToeGameJoinRes>()
                if (roomId.isNullOrBlank()) {
                    // The relocated Actor reconstructs the room from the durable accepted reply.
                    roomId = reply.state.roomId
                }
                joinGame(roomId.orEmpty())
                context.boundSession().kotlin().send(JoinGameNotify(reply.state)).await()
            }
            is ZLinkActorJoinCompletion.Rejected ->
                context
                    .boundSession()
                    .kotlin()
                    .send(JoinGameFailedNotify(roomId.orEmpty(), "Rejected"))
                    .await()
            is ZLinkActorJoinCompletion.Failed ->
                context
                    .boundSession()
                    .kotlin()
                    .send(JoinGameFailedNotify(roomId.orEmpty(), completion.kind().name))
                    .await()
        }
    }

    // --8<-- [end:doc-join-completed]

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
