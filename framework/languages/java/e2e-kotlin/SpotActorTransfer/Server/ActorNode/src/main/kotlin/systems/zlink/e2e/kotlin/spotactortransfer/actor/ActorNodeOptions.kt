package systems.zlink.e2e.kotlin.spotactortransfer.actor

import org.springframework.boot.context.properties.ConfigurationProperties

@ConfigurationProperties("e2e")
data class ActorNodeOptions(
    val nodeRid: String,
    val meshEndpoint: String,
    val meshPeers: String,
    val streamEndpoint: String,
    val httpEndpoint: String,
    val redisLocationEndpoint: String,
    val locationKeyPrefix: String,
    val logDirectory: String,
    val scenario: String,
)
