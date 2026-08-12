package systems.zlink.samples.kotlin.zoneworld.server.configuration

import org.springframework.boot.context.properties.ConfigurationProperties
@ConfigurationProperties("sample")
data class SampleTopology(
    val role: String? = null,
    val nodeId: String? = null,
    val meshEndpoint: String? = null,
    val streamEndpoint: String? = null,
    val redisEndpoint: String? = null,
    val redisKeyPrefix: String? = null,
) {
    fun isRole(expected: String) = role.equals(expected, ignoreCase = true)
    fun required(value: String?, name: String): String = value?.takeIf { it.isNotBlank() }
        ?: error("sample.$name is required")
    fun roleValue() = required(role, "role")
    fun nodeValue() = required(nodeId, "node-id")
    fun meshValue() = required(meshEndpoint, "mesh-endpoint")
    fun streamValue() = required(streamEndpoint, "stream-endpoint")
    fun redisValue() = required(redisEndpoint, "redis-endpoint")
    fun prefixValue() = required(redisKeyPrefix, "redis-key-prefix")

    fun validate() {
        val value = roleValue()
        require(value == "zone" || value == "gateway" || value == "ops") {
            "sample.role must be gateway, zone, or ops"
        }
        if (value == "zone") nodeValue()
        if (value != "zone") streamValue()
        meshValue(); redisValue(); prefixValue()
    }

    companion object {
        fun configPath(args: Array<String>): String {
            require(args.size == 2 && args[0] == "--config" && args[1].isNotBlank()) {
                "Usage: ZoneWorldServer --config <path>"
            }
            return args[1]
        }
    }
}
