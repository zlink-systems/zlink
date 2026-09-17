package systems.zlink.tutorial.client

import org.springframework.http.ResponseEntity
import org.springframework.web.bind.annotation.GetMapping
import org.springframework.web.bind.annotation.PathVariable
import org.springframework.web.bind.annotation.PostMapping
import org.springframework.web.bind.annotation.RestController
import systems.zlink.framework.channels.ZLinkRouteClient
import systems.zlink.framework.kotlin.kotlin
import systems.zlink.framework.kotlin.requestToChannel
import systems.zlink.tutorial.shared.GetPlayerProfile
import systems.zlink.tutorial.shared.IssueSessionTicket
import systems.zlink.tutorial.shared.PlayerProfile
import systems.zlink.tutorial.shared.RecordLogin
import systems.zlink.tutorial.shared.SessionTicket

@RestController
class PlayerEndpoints(route: ZLinkRouteClient) {

    // The Java ZLinkRouteClient is what Spring injects. kotlin() wraps it in the
    // Kotlin client, whose calls end in await() instead of submit(Class).
    private val route = route.kotlin()

    // --8<-- [start:channel-request-call]
    @GetMapping("/players/{playerId}/profile")
    suspend fun profile(@PathVariable playerId: String): PlayerProfile =
        // The target is a channel name. Which node answers is decided at call time.
        // The reply type is a reified type argument, not a Class passed at the end.
        route.requestToChannel<PlayerProfile>("profile", GetPlayerProfile(playerId))
            .await()
    // --8<-- [end:channel-request-call]

    // --8<-- [start:channel-send-call]
    @PostMapping("/players/{playerId}/logins")
    suspend fun recordLogin(@PathVariable playerId: String): ResponseEntity<Void> {
        // Returns as soon as the message is admitted locally, with no reply to
        // wait for.
        route.sendToChannel("profile", RecordLogin(playerId)).await()

        return ResponseEntity.accepted().build()
    }
    // --8<-- [end:channel-send-call]

    // --8<-- [start:clientserver-call]
    @PostMapping("/players/{playerId}/tickets")
    suspend fun issueTicket(@PathVariable playerId: String): String =
        // Same call shape as a mesh channel; only the routing differs.
        route.requestToChannel<SessionTicket>("ticketing", IssueSessionTicket(playerId))
            .await()
            .value
    // --8<-- [end:clientserver-call]
}
