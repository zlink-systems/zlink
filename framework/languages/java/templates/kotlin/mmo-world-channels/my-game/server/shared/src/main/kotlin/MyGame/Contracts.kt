package MyGame

import com.fasterxml.jackson.databind.annotation.JsonSerialize
import com.fasterxml.jackson.module.kotlin.jacksonObjectMapper
import java.util.Locale

object Json {
    val mapper = jacksonObjectMapper()
}

object Names {
    const val LANGUAGE = "kotlin"
    const val WORLD_A = "A"
    const val WORLD_B = "B"
    const val WORLD_LIST_PATH = "/worlds"
    const val STREAM = "players"
    const val PLAYER = "Player"
    const val ZONE = "Zone"
    const val PLAYER_KEY = "player:"
    const val FIELD = "field@"
    const val FIRST_CHANNEL = 1L
    const val UPDATE_CHANNEL = 3L
    val SCENARIOS = Scenario.entries.map { it.id }

    fun mesh(world: String) =
        when (world) {
            WORLD_A -> "my-game-world-a"
            WORLD_B -> "my-game-world-b"
            else -> error("Unknown world: $world")
        }

    fun zone(channel: Long): String {
        require(channel >= FIRST_CHANNEL) { "Channel is out of range" }
        return "$FIELD$channel"
    }

    fun channel(zone: String): Long {
        require(zone.startsWith(FIELD)) { "Invalid Zone ID" }
        val number = zone.removePrefix(FIELD).toLong()
        require(zone(number) == zone) { "Invalid channel" }
        return number
    }
}

enum class Scenario {
    WORLDS,
    CHANNEL,
    RECONNECT,
    UPDATE;

    val id: String
        get() = name.lowercase(Locale.ROOT)

    companion object {
        fun parse(id: String) =
            entries.singleOrNull { it.id == id } ?: error("Unknown scenario: $id")
    }
}

@JsonSerialize class SessionInfoRequest

@JsonSerialize class ViewRequest

object GameRules {
    const val MAX_INTEGER = 9_007_199_254_740_991L
    const val EARN_AMOUNT = 10L
    const val EARN_INTERVAL_MS = 1_000L
}

object Rejection {
    const val EARN_TOO_SOON = "earn-too-soon"
    const val GOLD_LIMIT = "gold-limit"
    const val INTEGER_OUT_OF_RANGE = "integer-out-of-range"
    const val PACKET_NOT_ALLOWED = "packet-not-allowed"
}

data class SessionInfo(
    val sessionId: String = "",
    val actorId: String = "",
    val objectGeneration: Long = 0,
)

data class EnterWorldRequest(val playerId: String = "")

@JsonSerialize class EarnRequest

@JsonSerialize class ChannelViewRequest

data class ChannelView(val channelId: Long = 0, val playerIds: List<String> = emptyList())

data class ActionRejected(val reason: String = "", val retryAfterMs: Int = 0)

data class ChangeChannelRequest(val channel: Long = 0)

data class PlayerView(
    val worldId: String = "",
    val channel: Long = 0,
    val nodeId: String = "",
    val gold: Long = 0,
)

data class WorldInfo(val worldId: String, val name: String, val sessionEndpoint: String)

data class NodeConfig(
    val id: String,
    val redisEndpoint: String? = null,
    val keyPrefix: String? = null,
    val meshEndpoint: String? = null,
    val streamEndpoint: String? = null,
    val worldId: String? = null,
    val zoneIds: List<String> = emptyList(),
    val zoneCapacity: Int = 0,
    val httpEndpoint: String? = null,
    val worlds: List<WorldInfo> = emptyList(),
)

object Roles {
    const val EXTERNAL = "external"
    const val ACCOUNTS = "accounts"
    const val SESSION = "session"
    const val CHANNEL = "channel"
    const val WORLD_DB = "world-db"
    const val LOCATION_STORE = "location-store"
    val APPLICATION = setOf(CHANNEL, SESSION, ACCOUNTS)
    val WITHOUT_SPOTS = setOf(EXTERNAL, ACCOUNTS, WORLD_DB, LOCATION_STORE)
}
