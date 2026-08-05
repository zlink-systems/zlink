package systems.zlink.e2e.kotlin.pubsub.client.Support

data class ClientOptions(
    val mode: String,
    val publisherHttp: String,
    val sub1Http: String,
    val sub2Http: String,
    val sub3Http: String,
    val reconnectHttp: String,
    val publisherBin: String,
    val subscriberBin: String,
    val publisherEndpoint: String,
    val redisLocationEndpoint: String,
    val locationKeyPrefix: String,
    val logDir: String,
    val publisherReadyFile: String,
    val prelateContinueFile: String,
    val lateReadyFile: String,
    val lateContinueFile: String,
) {
    companion object {
        fun parse(args: Array<String>): ClientOptions {
            val values = parseArgs(args)
            fun required(key: String): String =
                values[key]?.takeIf { it.isNotBlank() } ?: throw IllegalArgumentException("--$key is required.")
            return ClientOptions(
                mode = values["mode"]?.takeIf { it.isNotBlank() } ?: "default",
                publisherHttp = required("publisher-http"),
                sub1Http = required("sub1-http"),
                sub2Http = required("sub2-http"),
                sub3Http = required("sub3-http"),
                reconnectHttp = values["reconnect-http"].orEmpty(),
                publisherBin = values["publisher-bin"].orEmpty(),
                subscriberBin = values["subscriber-bin"].orEmpty(),
                publisherEndpoint = values["publisher-endpoint"].orEmpty(),
                redisLocationEndpoint = values["redis-location-endpoint"].orEmpty(),
                locationKeyPrefix = values["location-key-prefix"].orEmpty(),
                logDir = values["log-dir"].orEmpty(),
                publisherReadyFile = values["publisher-ready-file"].orEmpty(),
                prelateContinueFile = values["prelate-continue-file"].orEmpty(),
                lateReadyFile = values["late-ready-file"].orEmpty(),
                lateContinueFile = values["late-continue-file"].orEmpty(),
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
