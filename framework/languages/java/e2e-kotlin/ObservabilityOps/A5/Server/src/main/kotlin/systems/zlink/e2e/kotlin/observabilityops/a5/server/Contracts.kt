package systems.zlink.e2e.kotlin.observabilityops.a5.server

object Contracts {
    const val CHANNEL = "observability.ops.kotlin.a5.request"
    const val HANDLER_GROUP = "observability-ops-kotlin-a5"

    @JvmRecord
    data class ProbeRequest(val value: String, val fail: Boolean)

    @JvmRecord
    data class ProbeReply(val value: String)
}
