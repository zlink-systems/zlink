package systems.zlink.samples.kotlin.bingo.server.configuration

import org.springframework.boot.context.properties.ConfigurationProperties
import org.springframework.boot.context.properties.bind.ConstructorBinding

@ConfigurationProperties("sample")
data class SampleTopology @ConstructorBinding constructor(
    val apiAChannelEndpoint: String,
    val apiBChannelEndpoint: String,
    val apiAMeshEndpoint: String,
    val apiBMeshEndpoint: String,
    val sessionARouterEndpoint: String,
    val sessionBRouterEndpoint: String,
    val playASpotRouterEndpoint: String,
    val playBSpotRouterEndpoint: String,
    val apiMatchmakingRouterEndpoint: String,
    val matchmakingRouterEndpoint: String,
    val sessionAStreamEndpoint: String,
    val sessionBStreamEndpoint: String,
    val redisEndpoint: String,
    val redisKeyPrefix: String,
    val apiNode: String,
    val playNode: String,
    val sessionNode: String,
    val logDirectory: String,
) {
    init {
        listOf(
            "apiAChannelEndpoint" to apiAChannelEndpoint,
            "apiBChannelEndpoint" to apiBChannelEndpoint,
            "apiAMeshEndpoint" to apiAMeshEndpoint,
            "apiBMeshEndpoint" to apiBMeshEndpoint,
            "sessionARouterEndpoint" to sessionARouterEndpoint,
            "sessionBRouterEndpoint" to sessionBRouterEndpoint,
            "playASpotRouterEndpoint" to playASpotRouterEndpoint,
            "playBSpotRouterEndpoint" to playBSpotRouterEndpoint,
            "apiMatchmakingRouterEndpoint" to apiMatchmakingRouterEndpoint,
            "matchmakingRouterEndpoint" to matchmakingRouterEndpoint,
            "sessionAStreamEndpoint" to sessionAStreamEndpoint,
            "sessionBStreamEndpoint" to sessionBStreamEndpoint,
            "redisEndpoint" to redisEndpoint,
            "redisKeyPrefix" to redisKeyPrefix,
            "logDirectory" to logDirectory,
        ).forEach { (name, value) -> require(value.isNotBlank()) { "sample.$name is required" } }
        require(apiNode in setOf("a", "b")) { "sample.apiNode must be a or b" }
        require(playNode in setOf("a", "b")) { "sample.playNode must be a or b" }
        require(sessionNode in setOf("a", "b")) { "sample.sessionNode must be a or b" }
    }

    fun selectedApiChannelEndpoint() = if (apiNode == "b") apiBChannelEndpoint else apiAChannelEndpoint
    fun selectedApiMeshEndpoint() = if (apiNode == "b") apiBMeshEndpoint else apiAMeshEndpoint
    fun selectedPlaySpotRouterEndpoint() = if (playNode == "b") playBSpotRouterEndpoint else playASpotRouterEndpoint
    fun selectedSessionRouterEndpoint() = if (sessionNode == "b") sessionBRouterEndpoint else sessionARouterEndpoint
    fun selectedStreamEndpoint() = if (sessionNode == "b") sessionBStreamEndpoint else sessionAStreamEndpoint

    companion object {
        fun configPath(args: Array<String>): String {
            require(args.size == 2 && args[0] == "--config" && args[1].isNotBlank()) {
                "Usage: <role executable> --config <path>"
            }
            return args[1]
        }
    }
}
