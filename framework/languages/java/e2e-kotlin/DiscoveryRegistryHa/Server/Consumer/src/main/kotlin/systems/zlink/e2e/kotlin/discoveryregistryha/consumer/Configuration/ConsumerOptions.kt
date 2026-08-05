package systems.zlink.e2e.kotlin.discoveryregistryha.consumer.Configuration

import systems.zlink.e2e.kotlin.discoveryregistryha.CliOptions

data class ConsumerOptions(
    val rid: String,
    val httpEndpoint: String,
    val redisLocationEndpoint: String,
    val locationKeyPrefix: String,
    val redisCommandTimeoutMillis: Long,
    val heartbeatMillis: Long,
    val leaseTtlMillis: Long,
    val pollingMillis: Long,
    val storeFailureGraceMillis: Long,
    val storeMode: String,
    val logDir: String,
) {
    companion object {
        fun parse(args: Array<String>): ConsumerOptions {
            val options = CliOptions.parse(args)
            return ConsumerOptions(
                rid = options.get("--consumer-rid", "consumer"),
                httpEndpoint = options.get("--http-endpoint"),
                redisLocationEndpoint = options.get("--redis-location-endpoint"),
                locationKeyPrefix = options.get("--location-key-prefix"),
                redisCommandTimeoutMillis = options.get("--redis-command-timeout-ms", "5000").toLong(),
                heartbeatMillis = options.get("--location-heartbeat-ms", "1000").toLong(),
                leaseTtlMillis = options.get("--location-lease-ttl-ms", "3000").toLong(),
                pollingMillis = options.get("--location-polling-ms", "500").toLong(),
                storeFailureGraceMillis = options.get("--location-store-failure-grace-ms", "6000").toLong(),
                storeMode = options.get("--location-store-mode", "watch"),
                logDir = options.get("--log-dir", "logs"),
            )
        }
    }
}
