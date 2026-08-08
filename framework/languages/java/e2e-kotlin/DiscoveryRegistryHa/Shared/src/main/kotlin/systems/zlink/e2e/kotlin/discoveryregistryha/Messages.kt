package systems.zlink.e2e.kotlin.discoveryregistryha

class Contracts private constructor() {
    companion object {
        const val CHANNEL: String = "discovery.registry.ha.api"
        const val HANDLER_GROUP: String = "discovery-registry-ha"
        const val OBJECT_TYPE: String = "discovery-registry-ha-object"
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

    @JvmRecord
    data class ObjectReq(
        val spotId: String,
        val marker: String,
    )

    @JvmRecord
    data class ObjectRes(
        val spotId: String,
        val ownerRid: String,
        val objectGeneration: Long,
    )

    @JvmRecord
    data class ObjectOutcome(
        val succeeded: Boolean,
        val reply: ObjectRes?,
        val errorKind: String,
        val errorMessage: String,
    )
}
