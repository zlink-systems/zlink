package systems.zlink.e2e.kotlin.registrymessaging.consumer.Configuration

data class ConsumerOptions(
    val httpUrl: String,
    val logDir: String,
    val traceLabel: String,
    val providerEndpoints: List<String>,
    val redisLocationEndpoint: String,
    val locationKeyPrefix: String,
) {
    companion object {
        fun parse(args: Array<String>): ConsumerOptions {
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
            val endpoints = values["provider-endpoint"]?.filter { it.isNotBlank() }.orEmpty()
            val redisLocationEndpoint = values.lastRequired("redis-location-endpoint")
            val locationKeyPrefix = values.lastRequired("location-key-prefix")
            return ConsumerOptions(
                httpUrl = values.last("http-url", "http://127.0.0.1:0"),
                logDir = values.last("log-dir", "logs"),
                traceLabel = values.last("trace-label", "consumer"),
                providerEndpoints = endpoints,
                redisLocationEndpoint = redisLocationEndpoint,
                locationKeyPrefix = locationKeyPrefix,
            )
        }
    }
}

private fun Map<String, List<String>>.last(key: String, defaultValue: String): String =
    this[key]?.lastOrNull()?.takeIf { it.isNotBlank() } ?: defaultValue

private fun Map<String, List<String>>.lastRequired(key: String): String =
    this[key]?.lastOrNull()?.takeIf { it.isNotBlank() }
        ?: throw IllegalArgumentException("--$key is required.")
