package systems.zlink.tutorial.server.spots

import systems.zlink.framework.kotlin.ZLinkSuspendingInstanceSpot
import systems.zlink.framework.kotlin.ZLinkSuspendingSpotRequestHandler
import systems.zlink.framework.kotlin.addHandler
import systems.zlink.framework.spots.ZLinkInstanceSpotContext
import systems.zlink.tutorial.shared.JoinMatchQueue
import systems.zlink.tutorial.shared.MatchQueueStatus

// --8<-- [start:instance-spot-class]
// Unlike a room, a match queue is never created explicitly. The first message
// addressed to a queue id brings it into being and is then handled by it.
// Players do not join it as members; it only processes requests, so there is
// no actor type to name and no create or join callback to write.
//
// ZLinkSuspendingInstanceSpot is the Kotlin base for this kind of Spot, the
// counterpart of ZLinkSuspendingSpot for a room.
class MatchQueue(override val context: ZLinkInstanceSpotContext) : ZLinkSuspendingInstanceSpot() {

    private val waiting = mutableListOf<String>()

    init {
        // Handlers are named here, the same way
        // the room names its own.
        context.handlers().addHandler<JoinMatchQueueHandler>()
    }

    fun waiting(): Int = waiting.size

    fun enqueue(playerId: String) {
        waiting.add(playerId)
    }
}

// --8<-- [end:instance-spot-class]

// --8<-- [start:instance-spot-handler]
// Handlers are written the same way as room
// handlers: the first type argument is the
// queue, the other two are request and reply.
class JoinMatchQueueHandler :
    ZLinkSuspendingSpotRequestHandler<MatchQueue, JoinMatchQueue, MatchQueueStatus> {

    override suspend fun handle(spot: MatchQueue, request: JoinMatchQueue): MatchQueueStatus {
        spot.enqueue(request.playerId)
        return MatchQueueStatus(spot.waiting())
    }
}
// --8<-- [end:instance-spot-handler]
