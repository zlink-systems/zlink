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
    const val FIRST_CHANNEL = 1
    const val UPDATE_CHANNEL = 3
    val SCENARIOS = Scenario.entries.map { it.id }

    fun mesh(world: String) =
        when (world) {
            WORLD_A -> "my-game-world-a"
            WORLD_B -> "my-game-world-b"
            else -> error("Unknown world: $world")
        }

    fun zone(channel: Int): String {
        require(channel >= FIRST_CHANNEL) { "Channel must be positive" }
        return "$FIELD$channel"
    }

    fun channel(zone: String): Int {
        require(zone.startsWith(FIELD)) { "Invalid Zone ID" }
        val number = zone.removePrefix(FIELD).toInt()
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

data class SessionInfo(
    val sessionId: String = "",
    val actorId: String = "",
    val objectGeneration: Long = 0,
)

data class EnterWorldRequest(val playerId: String = "")

data class EarnRequest(val amount: Long = 0)

data class ChangeChannelRequest(val channel: Int = 0)

data class PlayerView(
    val worldId: String = "",
    val channel: Int = 0,
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
