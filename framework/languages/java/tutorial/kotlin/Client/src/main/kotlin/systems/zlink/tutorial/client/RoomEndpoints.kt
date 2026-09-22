package systems.zlink.tutorial.client

import java.time.Duration
import org.springframework.http.ResponseEntity
import org.springframework.web.bind.annotation.GetMapping
import org.springframework.web.bind.annotation.PathVariable
import org.springframework.web.bind.annotation.PostMapping
import org.springframework.web.bind.annotation.RequestBody
import org.springframework.web.bind.annotation.RestController
import systems.zlink.framework.channels.ZLinkRouteClient
import systems.zlink.framework.kotlin.kotlin
import systems.zlink.framework.kotlin.requestToSpot
import systems.zlink.framework.kotlin.sendToSpot
import systems.zlink.framework.spots.ZLinkSpotManager
import systems.zlink.tutorial.shared.GetRoomState
import systems.zlink.tutorial.shared.OpenRoom
import systems.zlink.tutorial.shared.PostChat
import systems.zlink.tutorial.shared.RoomState

@RestController
class RoomEndpoints(route: ZLinkRouteClient, rooms: ZLinkSpotManager) {

    // The Java ZLinkSpotManager is what Spring injects. kotlin() wraps it in the
    // Kotlin manager, whose calls end in await() instead of submit().
    private val rooms = rooms.kotlin()
    private val route = route.kotlin()

    // --8<-- [start:spot-create-call]
    @PostMapping("/rooms")
    suspend fun openRoom(@RequestBody request: OpenRoom): String =
        rooms
            .create("game-room") // Picks the factory and the candidate nodes.
            .inMesh("game")
            .request(request) // Reaches the room's create callback.
            .await()
            // From here on the room is addressed by this id alone.
            .spot()
            .spotId()

    // --8<-- [end:spot-create-call]

    // --8<-- [start:spot-message-call]
    // --8<-- [start:spot-send-call]
    @PostMapping("/rooms/{roomId}/chat")
    suspend fun postChat(
        @PathVariable roomId: String,
        @RequestBody message: PostChat,
    ): ResponseEntity<Void> {
        // The id is enough; the Framework resolves where the room currently runs.
        route.sendToSpot(roomId, message).await()

        return ResponseEntity.accepted().build()
    }

    // --8<-- [end:spot-send-call]

    // --8<-- [start:spot-request-call]
    @GetMapping("/rooms/{roomId}")
    suspend fun roomState(@PathVariable roomId: String): RoomState =
        route
            .requestToSpot<RoomState>(roomId, GetRoomState())
            .timeout(Duration.ofSeconds(3))
            .await()
    // --8<-- [end:spot-request-call]
    // --8<-- [end:spot-message-call]
}
