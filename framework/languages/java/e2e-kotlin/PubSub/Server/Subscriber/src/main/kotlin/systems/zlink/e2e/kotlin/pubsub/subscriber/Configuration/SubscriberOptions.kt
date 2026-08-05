package systems.zlink.e2e.kotlin.pubsub.subscriber

data class SubscriberOptions(
    val rid: String,
    val topics: Set<String>,
    val httpEndpoint: String,
    val handlerDelayMillis: Long?,
    val redisLocationEndpoint: String,
    val locationKeyPrefix: String,
    val logDir: String,
) {
    companion object {
        fun parse(args: Array<String>): SubscriberOptions {
            val values = parseArgs(args)
            fun required(key: String): String =
                values[key]?.takeIf { it.isNotBlank() } ?: throw IllegalArgumentException("--$key is required.")
            return SubscriberOptions(
                rid = values["rid"]?.takeIf { it.isNotBlank() } ?: "sub-1",
                topics = parseTopics(values["topics"] ?: "all"),
                httpEndpoint = required("http-endpoint"),
                handlerDelayMillis = values["handler-delay-ms"]?.takeIf { it.isNotBlank() }?.toLong(),
                redisLocationEndpoint = required("redis-location-endpoint"),
                locationKeyPrefix = required("location-key-prefix"),
                logDir = values["log-dir"]?.takeIf { it.isNotBlank() } ?: "logs",
            )
        }

        private fun parseTopics(value: String): Set<String> {
            val topics = value.split(",")
                .map { it.trim() }
                .filter { it.isNotEmpty() }
                .toMutableSet()
            topics += "all"
            return topics
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
