package systems.zlink.samples.kotlin.tictactoe.server.api.handlers

import kotlinx.coroutines.future.await
import org.springframework.web.bind.annotation.PostMapping
import org.springframework.web.bind.annotation.RequestBody
import org.springframework.web.bind.annotation.RestController
import systems.zlink.framework.spots.ZLinkSpotManager
import systems.zlink.samples.kotlin.tictactoe.server.configuration.SampleSettings
import systems.zlink.samples.kotlin.tictactoe.server.configuration.SampleNames
import systems.zlink.samples.kotlin.tictactoe.shared.contracts.CreateGameHttpReq
import systems.zlink.samples.kotlin.tictactoe.shared.contracts.CreateGameHttpRes
import systems.zlink.samples.kotlin.tictactoe.shared.contracts.PlayNodeInfo

@RestController
class CreateGameHttpHandler(
    private val spots: ZLinkSpotManager,
    private val settings: SampleSettings,
) {
    @PostMapping("/games")
    suspend fun handle(@RequestBody request: CreateGameHttpReq): CreateGameHttpRes {
        val gameName = request.gameName?.takeIf { it.isNotBlank() } ?: "tictactoe-game"
        // --8<-- [start:doc-create]
        val created = spots.create("tictactoe.game")  // 이 stable type을 등록한 node가 후보가 된다.
            .inMesh(SampleNames.SpotMesh)             // Spot을 만들 mesh를 고른다.
            .timeout(SampleNames.RequestTimeout)
            .submit()
            .await()                                  // Kotlin의 비동기 완료 terminal이다.
        // --8<-- [end:doc-create]
        return CreateGameHttpRes(
            roomId = created.spot.spotId,
            gameName = gameName,
            playEndpoints = settings.playEndpoints,
            playNodes = settings.playEndpoints.map(::PlayNodeInfo),
            requiredLevel = SampleNames.RequiredLevel,
        )
    }
}
