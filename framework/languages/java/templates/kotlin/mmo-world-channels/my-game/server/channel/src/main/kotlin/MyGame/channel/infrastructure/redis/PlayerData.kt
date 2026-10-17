package MyGame.channel.infrastructure.redis

import MyGame.*
import MyGame.channel.application.PlayerRepository
import io.lettuce.core.RedisClient
import io.lettuce.core.api.StatefulRedisConnection
import java.util.concurrent.CompletionStage
import org.springframework.stereotype.Component

@Component
class PlayerData(config: NodeConfig) : PlayerRepository, AutoCloseable {
    private val prefix = requireNotNull(config.keyPrefix) + Names.PLAYER_KEY
    private val client = RedisClient.create("redis://${requireNotNull(config.redisEndpoint)}")
    private val connection: StatefulRedisConnection<String, String> =
        try {
            client.connect()
        } catch (error: RuntimeException) {
            client.shutdown()
            throw error
        }

    override fun load(id: String): CompletionStage<Long> =
        connection.async().get(prefix + id).thenApply { it?.toLong() ?: 0L }

    override fun save(id: String, gold: Long): CompletionStage<Void> =
        connection.async().set(prefix + id, gold.toString()).thenApply { null }

    override fun close() {
        try {
            connection.close()
        } finally {
            client.shutdown()
        }
    }
}
