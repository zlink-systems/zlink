package systems.zlink.tutorial.client

import java.time.Duration
import kotlinx.coroutines.future.await
import org.springframework.http.ResponseEntity
import org.springframework.web.bind.annotation.GetMapping
import org.springframework.web.bind.annotation.PathVariable
import org.springframework.web.bind.annotation.PostMapping
import org.springframework.web.bind.annotation.RequestBody
import org.springframework.web.bind.annotation.RestController
import systems.zlink.framework.actors.ZLinkActorClient
import systems.zlink.framework.actors.ZLinkActorCreateResult
import systems.zlink.framework.actors.ZLinkActorManager
import systems.zlink.tutorial.shared.ChangeNickname
import systems.zlink.tutorial.shared.CreatePlayer
import systems.zlink.tutorial.shared.GetPlayer
import systems.zlink.tutorial.shared.PlayerInfo

@RestController
class PlayerActorEndpoints(
    private val playerManager: ZLinkActorManager,
    private val players: ZLinkActorClient,
) {

    // --8<-- [start:actor-create-call]
    @PostMapping("/players/{playerId}")
    suspend fun createPlayer(
        @PathVariable playerId: String,
        @RequestBody request: CreatePlayer,
    ): String {
        // getOrCreate returns the existing player if there is one. The caller
        // does not choose which node hosts it.
        val result =
            playerManager
                .getOrCreate(playerId, "player")
                .inMesh("game")
                .request(request)
                .timeout(Duration.ofSeconds(10))
                .submit()
                .await()

        return when (result) {
            is ZLinkActorCreateResult.Existing -> "existing"
            is ZLinkActorCreateResult.Created -> "created"
            is ZLinkActorCreateResult.Rejected -> "rejected"
        }
    }

    // --8<-- [end:actor-create-call]

    // --8<-- [start:actor-send-call]
    @PostMapping("/players/{playerId}/nickname")
    suspend fun changeNickname(
        @PathVariable playerId: String,
        @RequestBody message: ChangeNickname,
    ): ResponseEntity<Void> {
        // Addressed by player id, like a room is by room id.
        players.sendToActor(playerId, message).submit().await()

        return ResponseEntity.accepted().build()
    }

    // --8<-- [end:actor-send-call]

    // --8<-- [start:actor-request-call]
    @GetMapping("/players/{playerId}")
    suspend fun getPlayer(@PathVariable playerId: String): PlayerInfo =
        players
            .requestToActor(playerId, GetPlayer())
            .timeout(Duration.ofSeconds(3))
            .submit(PlayerInfo::class.java)
            .await()
    // --8<-- [end:actor-request-call]
}
