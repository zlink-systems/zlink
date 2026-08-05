package systems.zlink.e2e.kotlin.registrymessaging.provider.Configuration

data class ServerOptions(
    val rid: String,
    val instanceId: String,
    val httpUrl: String,
    val logDir: String,
    val evidenceFile: String?,
    val redisLocationEndpoint: String,
    val locationKeyPrefix: String,
    val channelEndpoint: String?,
    val routeEndpoint: String?,
    val routePeerRids: List<String>,
    val weight: Int,
    val routePeers: List<String>,
) {
    companion object {
        fun parse(args: Array<String>): ServerOptions {
            val values = linkedMapOf<String, MutableList<String>>()
            var index = 0
            while (index < args.size) {
                val key = args[index]
                if (!key.startsWith("--")) {
                    index++
                    continue
                }
                require(index + 1 < args.size) { "Missing value for $key." }
                values.getOrPut(key.removePrefix("--")) { mutableListOf() }.add(args[index + 1])
                index += 2
            }
            val rid = values.last("rid", "node")
            return ServerOptions(
                rid = rid,
                instanceId = values.last("instance-id", rid),
                httpUrl = values.last("http-url", "http://127.0.0.1:0"),
                logDir = values.last("log-dir", "logs"),
                evidenceFile = values.lastOrNull("evidence-file"),
                redisLocationEndpoint = values.lastRequired("redis-location-endpoint"),
                locationKeyPrefix = values.lastRequired("location-key-prefix"),
                channelEndpoint = values.lastOrNull("channel-endpoint"),
                routeEndpoint = values.lastOrNull("route-endpoint"),
                routePeerRids = values["route-peer-rid"]?.toList().orEmpty(),
                weight = values.last("weight", "100").toInt(),
                routePeers = values["route-peer"]?.toList().orEmpty(),
            )
        }
    }
}

private fun Map<String, List<String>>.last(key: String, defaultValue: String): String =
    this[key]?.lastOrNull()?.takeIf { it.isNotBlank() } ?: defaultValue

private fun Map<String, List<String>>.lastOrNull(key: String): String? =
    this[key]?.lastOrNull()?.takeIf { it.isNotBlank() }

private fun Map<String, List<String>>.lastRequired(key: String): String =
    lastOrNull(key) ?: throw IllegalArgumentException("--$key is required.")
