package systems.zlink.tutorial.client

import kotlinx.coroutines.future.await
import org.springframework.http.ResponseEntity
import org.springframework.web.bind.annotation.GetMapping
import org.springframework.web.bind.annotation.PathVariable
import org.springframework.web.bind.annotation.RestController
import systems.zlink.framework.actors.ZLinkActorManager
import systems.zlink.framework.spots.ZLinkSpotManager

@RestController
class LocationEndpoints(
    private val rooms: ZLinkSpotManager,
    private val players: ZLinkActorManager,
) {

    // --8<-- [start:location-find]
    // find answers from the Location Store alone: it reports where the object is,
    // and only while it is ready to receive. Nothing is sent to the object.
    @GetMapping("/locations/rooms/{roomId}")
    suspend fun findRoom(@PathVariable roomId: String): ResponseEntity<Map<String, String>> {
        val room =
            rooms.find(roomId).await().orElse(null) ?: return ResponseEntity.notFound().build()

        return ResponseEntity.ok(
            mapOf("spotId" to room.spotId(), "node" to room.nodeRid().toString())
        )
    }

    @GetMapping("/locations/players/{playerId}")
    suspend fun findPlayer(@PathVariable playerId: String): ResponseEntity<Map<String, String>> {
        val player =
            players.find(playerId).await().orElse(null) ?: return ResponseEntity.notFound().build()

        return ResponseEntity.ok(
            mapOf("actorId" to player.actorId(), "node" to player.nodeRid().toString())
        )
    }
    // --8<-- [end:location-find]
}
