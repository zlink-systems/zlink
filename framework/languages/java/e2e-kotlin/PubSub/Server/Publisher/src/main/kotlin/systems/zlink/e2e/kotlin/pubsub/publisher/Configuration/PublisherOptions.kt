package systems.zlink.e2e.kotlin.pubsub.publisher

data class PublisherOptions(
    val publisherEndpoint: String,
    val httpEndpoint: String,
    val routingId: String?,
    val routingIdPrefix: String?,
    val advertiseHost: String?,
    val channelName: String,
    val listenPort: Int?,
    val redisLocationEndpoint: String?,
    val locationKeyPrefix: String?,
    val logDir: String,
) {
    companion object {
        fun parse(args: Array<String>): PublisherOptions {
            val values = parseArgs(args)
            fun required(key: String): String =
                values[key]?.takeIf { it.isNotBlank() } ?: throw IllegalArgumentException("--$key is required.")
            return PublisherOptions(
                publisherEndpoint = values["publisher-endpoint"]?.takeIf { it.isNotBlank() }.orEmpty(),
                httpEndpoint = required("http-endpoint"),
                routingId = values["rid"]?.takeIf { it.isNotBlank() },
                routingIdPrefix = values["routing-id-prefix"]?.takeIf { it.isNotBlank() },
                advertiseHost = values["advertise-host"]?.takeIf { it.isNotBlank() },
                channelName = values["channel-name"]?.takeIf { it.isNotBlank() }
                    ?: systems.zlink.e2e.kotlin.pubsub.shared.Contracts.EVENT_CHANNEL,
                listenPort = values["publisher-port"]?.takeIf { it.isNotBlank() }?.toInt(),
                redisLocationEndpoint = values["redis-location-endpoint"]?.takeIf { it.isNotBlank() },
                locationKeyPrefix = values["location-key-prefix"]?.takeIf { it.isNotBlank() },
                logDir = values["log-dir"]?.takeIf { it.isNotBlank() } ?: "logs",
            ).also { options ->
                require(options.publisherEndpoint.isNotBlank() || options.listenPort != null) {
                    "--publisher-endpoint or --publisher-port is required."
                }
                require(options.redisLocationEndpoint == null || options.locationKeyPrefix != null) {
                    "--location-key-prefix is required with --redis-location-endpoint."
                }
            }
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
