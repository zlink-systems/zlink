package systems.zlink.samples.kotlin.tictactoe.server.configuration

import java.net.URI
import org.springframework.boot.context.properties.ConfigurationProperties

@ConfigurationProperties("sample")
data class SampleSettings(
    val apiBindUrl: String,
    val apiPublicUrl: String,
    val apiChannelEndpoint: String,
    val playEndpoint: String,
    val playEndpoints: List<String>,
    val routeEndpoint: String,
    val spotEndpoint: String,
    val spotEndpoints: List<String>,
    val spotPubSubEndpoint: String,
    val spotPubSubEndpoints: List<String>,
    val redisEndpoint: String,
    val redisKeyPrefix: String,
    val peerSpotEndpoint: String,
    val peerSpotPubSubEndpoint: String,
    val logDirectory: String,
) {
    init {
        require(apiBindUrl.isNotBlank()) { "sample.apiBindUrl is required" }
        require(apiPublicUrl.isNotBlank()) { "sample.apiPublicUrl is required" }
        require(apiChannelEndpoint.isNotBlank()) { "sample.apiChannelEndpoint is required" }
        require(playEndpoint.isNotBlank()) { "sample.playEndpoint is required" }
        require(spotEndpoint.isNotBlank()) { "sample.spotEndpoint is required" }
        require(spotPubSubEndpoint.isNotBlank()) { "sample.spotPubSubEndpoint is required" }
        require(redisEndpoint.isNotBlank()) { "sample.redisEndpoint is required" }
        require(redisKeyPrefix.isNotBlank()) { "sample.redisKeyPrefix is required" }
        require(logDirectory.isNotBlank()) { "sample.logDirectory is required" }
    }

    val apiHttpPort: Int
        get() = URI.create(apiBindUrl).port

    val playIndex: Int
        get() = playEndpoints.indexOf(playEndpoint).takeIf { it >= 0 } ?: 0

    companion object {
        fun configPath(args: Array<String>): String {
            require(args.size == 2 && args[0] == "--config" && args[1].isNotBlank()) {
                "Usage: <role executable> --config <path>"
            }
            return args[1]
        }
    }
}
