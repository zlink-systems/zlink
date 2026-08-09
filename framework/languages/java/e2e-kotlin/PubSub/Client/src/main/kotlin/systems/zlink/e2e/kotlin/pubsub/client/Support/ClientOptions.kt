package Support

import systems.zlink.e2e.kotlin.pubsub.client.Support
data class ClientOptions(
    val mode: String,
    val publisherHttp: String,
    val publisher2Http: String,
    val publisher2Endpoint: String,
    val publisher2Port: Int?,
    val publisher2Rid: String,
    val publisher2NoStore: Boolean,
    val publisher2AdvertiseHost: String,
    val auditPublisherHttp: String,
    val auditPublisherEndpoint: String,
    val sub1Http: String,
    val sub2Http: String,
    val sub3Http: String,
    val sub4Http: String,
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
    val publisherPid: Long?,
) {
    companion object {
        fun parse(args: Array<String>): ClientOptions {
            val values = parseArgs(args)
            fun required(key: String): String =
                values[key]?.takeIf { it.isNotBlank() } ?: throw IllegalArgumentException("--$key is required.")
            return ClientOptions(
                mode = values["mode"]?.takeIf { it.isNotBlank() } ?: "default",
                publisherHttp = required("publisher-http"),
                publisher2Http = values["publisher2-http"].orEmpty(),
                publisher2Endpoint = values["publisher2-endpoint"].orEmpty(),
                publisher2Port = values["publisher2-port"]?.toIntOrNull(),
                publisher2Rid = values["publisher2-rid"]?.takeIf { it.isNotBlank() } ?: "publisher-b",
                publisher2NoStore = values["publisher2-no-store"]?.toBoolean() ?: false,
                publisher2AdvertiseHost = values["publisher2-advertise-host"].orEmpty(),
                auditPublisherHttp = values["audit-publisher-http"].orEmpty(),
                auditPublisherEndpoint = values["audit-publisher-endpoint"].orEmpty(),
                sub1Http = required("sub1-http"),
                sub2Http = required("sub2-http"),
                sub3Http = required("sub3-http"),
                sub4Http = values["sub4-http"].orEmpty(),
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
                publisherPid = values["publisher-pid"]?.toLongOrNull(),
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
