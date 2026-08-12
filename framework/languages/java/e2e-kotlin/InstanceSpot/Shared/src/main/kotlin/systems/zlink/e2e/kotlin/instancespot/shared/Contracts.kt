package systems.zlink.e2e.kotlin.instancespot.shared

class Contracts private constructor() {
    companion object {
        const val MESH = "instance-spot"
        const val HANDLER_GROUP = "instance-spot"
        const val STABLE_TYPE = "instance-echo"
    }

    @JvmRecord
    data class InstanceReq(
        val spotId: String,
        val operationId: String,
        val payload: String,
        val timeoutMilliseconds: Long,
    )

    @JvmRecord
    data class InstanceMsg(
        val spotId: String,
        val operationId: String,
        val payload: String,
    )

    @JvmRecord
    data class CloseMsg(
        val spotId: String,
        val operationId: String,
        val gateId: String,
    )

    @JvmRecord
    data class GateReq(val gateId: String, val open: Boolean)

    @JvmRecord
    data class InstanceRes(
        val spotId: String,
        val operationId: String,
        val payload: String,
        val ownerRid: String,
        val ownerLifecycle: String,
        val objectGeneration: Long,
        val handlerSequence: Long,
    )

    @JvmRecord
    data class InstanceCallRes(
        val succeeded: Boolean,
        val reply: InstanceRes?,
        val errorKind: String,
        val errorMessage: String,
    )

    @JvmRecord
    data class SendSubmitRes(
        val succeeded: Boolean,
        val errorKind: String,
        val errorMessage: String,
    )

    @JvmRecord
    data class ConcurrentReq(
        val spotId: String,
        val count: Int,
        val operationPrefix: String,
        val timeoutMilliseconds: Long,
    )

    @JvmRecord
    data class ConcurrentRes(val outcomes: List<InstanceCallRes>)

    @JvmRecord
    data class EvidenceEntry(
        val sequence: Long,
        val kind: String,
        val spotId: String,
        val operationId: String,
        val payload: String,
        val ownerRid: String,
        val lifecycleId: String,
        val objectGeneration: Long,
        val activeHandlers: Int,
        val detail: String,
    )

    @JvmRecord
    data class EvidenceSnapshot(
        val rid: String,
        val lifecycleId: String,
        val events: List<EvidenceEntry>,
    )

    @JvmRecord
    data class EvidenceWaitReq(
        val kind: String,
        val operationId: String,
        val timeoutMilliseconds: Long,
    )

    @JvmRecord
    data class EvidenceWaitRes(val found: Boolean, val snapshot: EvidenceSnapshot)

    @JvmRecord
    data class LookupRes(
        val found: Boolean,
        val spotId: String,
        val objectGeneration: Long,
        val meshName: String,
        val nodeRid: String,
        val errorKind: String,
        val errorMessage: String,
    )
}
