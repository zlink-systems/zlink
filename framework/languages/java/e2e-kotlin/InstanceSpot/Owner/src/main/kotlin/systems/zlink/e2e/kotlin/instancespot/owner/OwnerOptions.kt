package systems.zlink.e2e.kotlin.instancespot.owner

import systems.zlink.e2e.kotlin.instancespot.shared.Env

data class OwnerOptions(
    val rid: String,
    val lifecycleId: String,
    val httpEndpoint: String,
    val meshEndpoint: String,
    val redisLocationEndpoint: String,
    val locationKeyPrefix: String,
    val redisCommandTimeoutMillis: Long,
    val heartbeatMillis: Long,
    val leaseTtlMillis: Long,
    val pollingMillis: Long,
    val storeFailureGraceMillis: Long,
    val stableTypeLimit: Int,
    val disableRelocation: Boolean,
    val evidenceFile: String,
    val logDir: String,
) {
    companion object {
        fun fromEnv() = OwnerOptions(
            required("e2e.rid"),
            required("e2e.lifecycle-id"),
            required("e2e.http-endpoint"),
            required("e2e.mesh-endpoint"),
            required("e2e.redis-location-endpoint"),
            required("e2e.location-key-prefix"),
            positive("e2e.redis-command-timeout-millis"),
            positive("e2e.heartbeat-millis"),
            positive("e2e.lease-ttl-millis"),
            positive("e2e.polling-millis"),
            positive("e2e.store-failure-grace-millis"),
            Env.get("e2e.stable-type-limit", "0").toInt().also { require(it >= 0) },
            Env.get("e2e.disable-relocation", "true").toBoolean(),
            Env.get("e2e.evidence-file"),
            required("e2e.log-dir"),
        )

        private fun required(name: String): String = Env.get(name).also {
            require(it.isNotBlank()) { "$name is required" }
        }

        private fun positive(name: String): Long = Env.get(name).toLong().also {
            require(it > 0) { "$name must be positive" }
        }
    }
}
