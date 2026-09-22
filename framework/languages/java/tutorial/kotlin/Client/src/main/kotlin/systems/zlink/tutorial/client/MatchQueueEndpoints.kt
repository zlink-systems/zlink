package systems.zlink.tutorial.client

import java.time.Duration
import org.springframework.web.bind.annotation.PathVariable
import org.springframework.web.bind.annotation.PostMapping
import org.springframework.web.bind.annotation.RequestBody
import org.springframework.web.bind.annotation.RestController
import systems.zlink.framework.channels.ZLinkRouteClient
import systems.zlink.framework.kotlin.kotlin
import systems.zlink.framework.kotlin.requestToSpot
import systems.zlink.tutorial.shared.JoinMatchQueue
import systems.zlink.tutorial.shared.MatchQueueStatus

@RestController
class MatchQueueEndpoints(route: ZLinkRouteClient) {

    private val route = route.kotlin()

    // --8<-- [start:instance-spot-call]
    @PostMapping("/match-queues/{mode}")
    suspend fun joinMatchQueue(
        @PathVariable mode: String,
        @RequestBody request: JoinMatchQueue,
    ): MatchQueueStatus =
        // No create call: the first message for
        // this id brings the queue into being and
        // is then handled by it. Like the room
        // calls, this uses the Kotlin Spot wrapper.
        route
            .requestToSpot<MatchQueueStatus>(mode, request)
            .instanceSpot("match-queue")
            .inMesh("game")
            .timeout(Duration.ofSeconds(3))
            .await()
    // --8<-- [end:instance-spot-call]
}
