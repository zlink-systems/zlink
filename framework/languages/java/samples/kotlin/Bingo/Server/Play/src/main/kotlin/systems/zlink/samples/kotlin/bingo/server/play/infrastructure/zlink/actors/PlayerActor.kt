package systems.zlink.samples.kotlin.bingo.server.play.infrastructure.zlink.actors

import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionStage
import systems.zlink.framework.actors.ZLinkActorJoinCompletion
import systems.zlink.framework.actors.ZLinkActorJoinOperationId
import systems.zlink.framework.actors.ZLinkActor
import systems.zlink.framework.actors.ZLinkActorContext
import systems.zlink.samples.kotlin.bingo.shared.contracts.BingoRoomJoinRes
import systems.zlink.samples.kotlin.bingo.shared.contracts.MatchBingoRes

class PlayerActor(
    private val actorId: String,
    private val context: ZLinkActorContext,
) : ZLinkActor {
    var displayName: String = actorId
        private set
    var roomId: String = ""
        private set
    private var pendingRoomId: String? = null
    private val completedJoinOperations = mutableSetOf<ZLinkActorJoinOperationId>()
    var destroyAfterEntrySpotJoin: Boolean = false
        private set
    var disconnected: Boolean = false
        private set

    fun actorId(): String = actorId

    override fun context(): ZLinkActorContext = context

    fun setDisplayName(value: String) {
        displayName = value
    }

    fun joinRoom(value: String) {
        roomId = value
    }

    fun trackDeferredJoin(value: String) {
        check(pendingRoomId == null) { "a room join is already pending" }
        pendingRoomId = value
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

        var matchedRoomId = pendingRoomId
        pendingRoomId = null
        if (completion !is ZLinkActorJoinCompletion.Accepted) {
            return CompletableFuture.completedFuture(null)
        }

        val joined = completion.reply().decode(BingoRoomJoinRes::class.java)
        if (matchedRoomId.isNullOrBlank()) {
            matchedRoomId = joined.state.roomId
        }
        joinRoom(matchedRoomId.orEmpty())
        return context.boundSession()
            .send(MatchBingoRes(matchedRoomId.orEmpty(), joined.state))
            .submit()
    }

    fun markForDestroyAfterRoomLeave() {
        destroyAfterEntrySpotJoin = true
    }

    fun markDisconnected() {
        disconnected = true
    }

    fun push(message: Any): CompletionStage<Void> {
        return context.boundSession()
            .send(message)
            .submit()
    }
}
