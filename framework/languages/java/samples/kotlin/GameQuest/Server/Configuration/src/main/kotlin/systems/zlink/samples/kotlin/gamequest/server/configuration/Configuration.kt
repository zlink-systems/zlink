package systems.zlink.samples.kotlin.gamequest.server.configuration

import com.fasterxml.jackson.databind.JavaType
import com.fasterxml.jackson.module.kotlin.jacksonObjectMapper
import com.fasterxml.jackson.module.kotlin.readValue
import io.lettuce.core.RedisClient
import io.lettuce.core.api.StatefulRedisConnection
import io.lettuce.core.api.sync.RedisCommands
import java.time.Duration
import org.springframework.boot.context.properties.ConfigurationProperties
import systems.zlink.framework.locations.redis.ZLinkRedisLocationOptions
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore
import systems.zlink.samples.kotlin.gamequest.shared.contracts.QuestProgress
import systems.zlink.samples.kotlin.gamequest.shared.contracts.StoredQuestEvent

object SampleNames {
    const val StreamNode = "gamequest-stream"
    const val PlayerQuestMesh = "gamequest.player-quests"
    const val PlayerQuestSpotType = "gamequest.player-quest"
    const val PlayerSessionActorType = "gamequest.player-session"
    const val CompletedMarker = "gamequest=completed"
    const val ServerEvidenceMarker = "gamequest-server-evidence=completed"

}

object SampleTimings {
    val RequestTimeout: Duration = Duration.ofSeconds(20)
    val ConnectTimeout: Duration = Duration.ofSeconds(5)
}

@ConfigurationProperties("sample")
data class SampleTopology(
    val instanceName: String? = null,
    val logDirectory: String? = null,
    val streamEndpoint: String? = null,
    val channelEndpoint: String? = null,
    val httpEndpoint: String? = null,
    val redisEndpoint: String? = null,
    val redisKeyPrefix: String? = null,
) {
    fun gameApi(): GameApi = GameApi(
        required(instanceName, "instanceName"),
        required(logDirectory, "logDirectory"),
        required(streamEndpoint, "streamEndpoint"),
        required(httpEndpoint, "httpEndpoint"),
    )

    fun questMission(): QuestMission = QuestMission(
        required(instanceName, "instanceName"),
        required(logDirectory, "logDirectory"),
        required(channelEndpoint, "channelEndpoint"),
        required(httpEndpoint, "httpEndpoint"),
    )

    fun location(): Location = Location(
        required(redisEndpoint, "redisEndpoint"),
        required(redisKeyPrefix, "redisKeyPrefix"),
    )

    data class GameApi(
        val instanceName: String,
        val logDirectory: String,
        val streamEndpoint: String,
        val httpEndpoint: String,
    )

    data class QuestMission(
        val instanceName: String,
        val logDirectory: String,
        val channelEndpoint: String,
        val httpEndpoint: String,
    )

    data class Location(val redisEndpoint: String, val redisKeyPrefix: String)

    companion object {
        fun configPath(args: Array<String>): String {
            require(args.size == 2 && args[0] == "--config" && args[1].isNotBlank()) {
                "Usage: <role executable> --config <path>"
            }
            return args[1]
        }

        private fun required(value: String?, name: String): String {
            require(!value.isNullOrBlank()) { "sample.$name is required" }
            return value
        }
    }
}

object SampleLocationStore {
    fun create(topology: SampleTopology): ZLinkRedisLocationStore {
        val location = topology.location()
        return ZLinkRedisLocationStore(
            ZLinkRedisLocationOptions()
                .setConnectionString(location.redisEndpoint)
                .setKeyPrefix("${location.redisKeyPrefix}locations:")
                .setCommandTimeout(Duration.ofMillis(500)),
        )
    }
}

class RedisSampleStore(topology: SampleTopology) : AutoCloseable {
    private val location = topology.location()
    private val json = jacksonObjectMapper()
    private val client: RedisClient = RedisClient.create(redisUri(location.redisEndpoint))
    private val connection: StatefulRedisConnection<String, String> = client.connect()
    private val redis: RedisCommands<String, String> = connection.sync()

    fun bind(playerId: String, apiName: String) {
        redis.sadd(key("binding-history"), "$playerId:$apiName")
        redis.sadd(key("active-bindings"), playerId)
    }

    fun unbind(playerId: String) {
        redis.srem(key("active-bindings"), playerId)
    }

    fun bindingHistory(): List<String> = redis.smembers(key("binding-history")).sorted()
    fun activeBindings(): List<String> = redis.smembers(key("active-bindings")).sorted()

    fun writeProjection(playerId: String, projection: List<QuestProgress>) {
        redis.set(key("projection:$playerId"), json.writeValueAsString(projection))
    }

    fun readProjection(playerId: String): List<QuestProgress> {
        val value = redis.get(key("projection:$playerId"))
        if (value.isNullOrBlank()) return emptyList()
        val type: JavaType = json.typeFactory.constructCollectionType(List::class.java, QuestProgress::class.java)
        return json.readValue(value, type)
    }

    fun appendQuestEvents(events: List<StoredQuestEvent>) {
        if (events.isNotEmpty()) redis.rpush(key("quest-events"), *events.map(json::writeValueAsString).toTypedArray())
    }

    fun readQuestEvents(): List<StoredQuestEvent> =
        redis.lrange(key("quest-events"), 0, -1).map { json.readValue<StoredQuestEvent>(it) }

    fun recordRehydrated(playerId: String) {
        redis.hincrby(key("owner-rehydrates"), playerId, 1)
    }

    fun rehydrates(): Map<String, String> = redis.hgetall(key("owner-rehydrates"))

    override fun close() {
        connection.close()
        client.shutdown()
    }

    private fun key(name: String): String = "${location.redisKeyPrefix}gamequest:$name"
    private fun redisUri(endpoint: String): String = if (endpoint.startsWith("redis://")) endpoint else "redis://$endpoint"
}
