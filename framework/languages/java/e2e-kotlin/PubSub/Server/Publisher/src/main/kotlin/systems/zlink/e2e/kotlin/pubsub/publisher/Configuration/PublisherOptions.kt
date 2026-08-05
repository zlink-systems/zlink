package systems.zlink.e2e.kotlin.pubsub.publisher

data class PublisherOptions(
    val publisherEndpoint: String,
    val httpEndpoint: String,
    val redisLocationEndpoint: String,
    val locationKeyPrefix: String,
    val logDir: String,
) {
    companion object {
        fun parse(args: Array<String>): PublisherOptions {
            val values = parseArgs(args)
            fun required(key: String): String =
                values[key]?.takeIf { it.isNotBlank() } ?: throw IllegalArgumentException("--$key is required.")
            return PublisherOptions(
                publisherEndpoint = required("publisher-endpoint"),
                httpEndpoint = required("http-endpoint"),
                redisLocationEndpoint = required("redis-location-endpoint"),
                locationKeyPrefix = required("location-key-prefix"),
                logDir = values["log-dir"]?.takeIf { it.isNotBlank() } ?: "logs",
            )
        }
    }
}

private fun parseArgs(args: Array<String>): Map<String, String> {
    val values = linkedMapOf<String, String>()
    var index = 0
    while (index < args.size) {
        val key = args[index]
        if (!key.startsWith("--")) {
            index++
            continue
        }
        require(index + 1 < args.size) { "Missing value for $key." }
        values[key.removePrefix("--")] = args[index + 1]
        index += 2
    }
    return values
}
