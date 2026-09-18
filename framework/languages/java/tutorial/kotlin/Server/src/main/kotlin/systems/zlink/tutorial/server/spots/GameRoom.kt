package systems.zlink.tutorial.server.spots

import systems.zlink.framework.actors.ZLinkActor
import systems.zlink.framework.kotlin.addHandler
import systems.zlink.framework.kotlin.ZLinkSuspendingSpot
import systems.zlink.framework.kotlin.ZLinkSuspendingSpotPacketHandler
import systems.zlink.framework.kotlin.ZLinkSuspendingSpotRequestHandler
import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.framework.spots.ZLinkSpotActorJoinResult
import systems.zlink.framework.spots.ZLinkSpotContext
import systems.zlink.framework.spots.ZLinkSpotCreateResponse
import systems.zlink.tutorial.shared.GetRoomState
import systems.zlink.tutorial.shared.OpenRoom
import systems.zlink.tutorial.shared.PostChat
import systems.zlink.tutorial.shared.RoomState

// --8<-- [start:spot-class]
// A room owns its own state and is addressed by a global SpotId. Messages sent
// to one room run one at a time, so the fields below need no synchronization.
//
// ZLinkSuspendingSpot is the Kotlin base: it turns every CompletionStage
// callback into a suspending one. Its type argument is the actor type the room
// can admit; this room admits none, so it names the base type and rejects every
// join. Actors are a later chapter.
class GameRoom(override val context: ZLinkSpotContext) : ZLinkSuspendingSpot<ZLinkActor>() {

    private val chat = mutableListOf<String>()
    private var title = "untitled"

    init {
        // --8<-- [start:spot-handlers]
        // Handler classes are named here rather than scanned, the same way the
        // channel registrations name their group. Each takes the target room as
        // its first argument.
        context.handlers().addHandler<PostChatHandler>()
        context.handlers().addHandler<GetRoomStateHandler>()
        // --8<-- [end:spot-handlers]
    }

    // Runs before the room accepts any message. Rejecting here means the create
    // call fails and no room exists. Omit this method to accept every request.
    override suspend fun onCreateSuspending(request: ZLinkMessage): ZLinkSpotCreateResponse {
        title = request.decode(OpenRoom::class.java).title
        return ZLinkSpotCreateResponse.accept()
    }

    // No actor ever joins this room, so the three membership callbacks below say
    // so and do nothing else.
    override suspend fun onActorJoinSuspending(
        actorId: String,
        request: ZLinkMessage,
    ): ZLinkSpotActorJoinResult = ZLinkSpotActorJoinResult.reject()

    override suspend fun onJoinedActorSuspending(actor: ZLinkActor) {
    }

    override suspend fun onLeaveActorSuspending(actor: ZLinkActor) {
    }

    fun append(line: String) {
        chat.add(line)
    }

    fun state(): RoomState = RoomState(title, chat.toList())
}
// --8<-- [end:spot-class]

// --8<-- [start:spot-handler-classes]
// The two type arguments are what the Framework matches a packet against, so no
// packet name is written anywhere.
class PostChatHandler : ZLinkSuspendingSpotPacketHandler<GameRoom, PostChat> {
    override suspend fun handle(spot: GameRoom, message: PostChat) {
        spot.append("${message.playerId}: ${message.text}")
    }
}

// The third type argument is the reply. This handler only reads.
class GetRoomStateHandler : ZLinkSuspendingSpotRequestHandler<GameRoom, GetRoomState, RoomState> {
    override suspend fun handle(spot: GameRoom, request: GetRoomState): RoomState = spot.state()
}
// --8<-- [end:spot-handler-classes]
