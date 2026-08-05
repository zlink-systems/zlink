package systems.zlink.e2e.kotlin.discoveryregistryha

class Contracts private constructor() {
    companion object {
        const val CHANNEL: String = "discovery.registry.ha.api"
        const val HANDLER_GROUP: String = "discovery-registry-ha"
    }

    @JvmRecord
    data class WorkReq(val value: String)

    @JvmRecord
    data class WorkRes(
        val value: String,
        val providerRid: String,
    )

    @JvmRecord
    data class StoreDelayReq(val delayMilliseconds: Int)
}
