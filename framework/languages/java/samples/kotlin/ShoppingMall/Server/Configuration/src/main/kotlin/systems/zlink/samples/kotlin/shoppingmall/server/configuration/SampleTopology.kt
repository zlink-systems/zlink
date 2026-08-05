package systems.zlink.samples.kotlin.shoppingmall.server.configuration

import java.time.Duration
import org.springframework.boot.context.properties.ConfigurationProperties
import systems.zlink.framework.locations.redis.ZLinkRedisLocationOptions
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.OrderState
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.OrderStatuses

@ConfigurationProperties("sample")
data class SampleTopology(
    val instanceId: String? = null,
    val logDirectory: String? = null,
    val channelEndpoint: String? = null,
    val redisEndpoint: String? = null,
    val redisKeyPrefix: String? = null,
    val storeDirectory: String? = null,
) {
    fun role(): Role = Role(
        required(instanceId, "instanceId"),
        required(logDirectory, "logDirectory"),
        required(channelEndpoint, "channelEndpoint"),
        required(storeDirectory, "storeDirectory"),
    )

    fun location(): Location = Location(
        required(redisEndpoint, "redisEndpoint"),
        required(redisKeyPrefix, "redisKeyPrefix"),
    )

    fun requiredStoreDirectory(): String = required(storeDirectory, "storeDirectory")

    fun failedPlaceholder(orderId: String): OrderState =
        OrderState(orderId, OrderStatuses.Failed, null, null, null, "order does not exist", null, null,
            System.currentTimeMillis())

    data class Role(
        val instanceId: String,
        val logDirectory: String,
        val channelEndpoint: String,
        val storeDirectory: String,
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
