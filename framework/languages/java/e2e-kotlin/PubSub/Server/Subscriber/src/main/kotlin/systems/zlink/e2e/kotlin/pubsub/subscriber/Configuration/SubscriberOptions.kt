package systems.zlink.e2e.kotlin.pubsub.subscriber

data class SubscriberOptions(
    val rid: String,
    val topics: Set<String>,
    val httpEndpoint: String,
    val handlerDelayMillis: Long?,
    val includeAllTopics: Boolean,
    val manualEndpoint: String?,
    val mixedMode: Boolean,
    val redisLocationEndpoint: String?,
    val locationKeyPrefix: String?,
    val logDir: String,
) {
    companion object {
        fun parse(args: Array<String>): SubscriberOptions {
            val values = parseArgs(args)
            fun required(key: String): String =
                values[key]?.takeIf { it.isNotBlank() } ?: throw IllegalArgumentException("--$key is required.")
            return SubscriberOptions(
                rid = values["rid"]?.takeIf { it.isNotBlank() } ?: "sub-1",
                topics = parseTopics(
                    values["topics"] ?: "all",
                    values["include-all"]?.toBoolean() ?: true,
                ),
                httpEndpoint = required("http-endpoint"),
                handlerDelayMillis = values["handler-delay-ms"]?.takeIf { it.isNotBlank() }?.toLong(),
                includeAllTopics = values["include-all"]?.toBoolean() ?: true,
                manualEndpoint = values["manual-endpoint"]?.takeIf { it.isNotBlank() },
                mixedMode = values["mixed-mode"]?.toBoolean() ?: false,
                redisLocationEndpoint = values["redis-location-endpoint"]?.takeIf { it.isNotBlank() },
                locationKeyPrefix = values["location-key-prefix"]?.takeIf { it.isNotBlank() },
                logDir = values["log-dir"]?.takeIf { it.isNotBlank() } ?: "logs",
            ).also { options ->
                require(options.manualEndpoint != null || options.redisLocationEndpoint != null) {
                    "automatic subscriber requires --redis-location-endpoint or --manual-endpoint."
                }
                require(!options.mixedMode || options.manualEndpoint != null) {
                    "--mixed-mode requires --manual-endpoint."
                }
                require(options.redisLocationEndpoint == null || options.locationKeyPrefix != null) {
                    "--location-key-prefix is required with --redis-location-endpoint."
                }
            }
        }

        private fun parseTopics(value: String, includeAll: Boolean): Set<String> {
            val topics = value.split(",")
                .map { it.trim() }
                .filter { it.isNotEmpty() }
                .toMutableSet()
            if (includeAll) {
                topics += "all"
            }
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
