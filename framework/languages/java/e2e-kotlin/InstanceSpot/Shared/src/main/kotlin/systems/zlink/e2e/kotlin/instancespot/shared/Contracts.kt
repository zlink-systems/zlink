package systems.zlink.e2e.kotlin.instancespot.shared

class Contracts private constructor() {
    companion object {
        const val MESH = "instance-spot"
        const val HANDLER_GROUP = "instance-spot"
        const val STABLE_TYPE = "instance-echo"
    }

    @JvmRecord
    data class InstanceRequest(
        val spotId: String,
        val operationId: String,
        val payload: String,
        val timeoutMilliseconds: Long,
    )

    @JvmRecord
    data class InstanceSend(
        val spotId: String,
        val operationId: String,
        val payload: String,
    )

    @JvmRecord
    data class CloseRequest(
        val spotId: String,
        val operationId: String,
        val gateId: String,
    )

    @JvmRecord
    data class GateRequest(val gateId: String, val open: Boolean)

    @JvmRecord
    data class InstanceReply(
        val spotId: String,
        val operationId: String,
        val payload: String,
        val ownerRid: String,
        val ownerLifecycle: String,
        val objectGeneration: Long,
        val handlerSequence: Long,
    )

    @JvmRecord
    data class RequestOutcome(
        val succeeded: Boolean,
        val reply: InstanceReply?,
        val errorKind: String,
        val errorMessage: String,
    )

    @JvmRecord
    data class SendOutcome(
        val succeeded: Boolean,
        val errorKind: String,
        val errorMessage: String,
    )

    @JvmRecord
    data class ConcurrentRequest(
        val spotId: String,
        val count: Int,
        val operationPrefix: String,
        val timeoutMilliseconds: Long,
    )

    @JvmRecord
    data class ConcurrentOutcome(val outcomes: List<RequestOutcome>)

    @JvmRecord
    data class EvidenceEvent(
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
        val events: List<EvidenceEvent>,
    )

    @JvmRecord
    data class EvidenceWaitRequest(
        val kind: String,
        val operationId: String,
        val timeoutMilliseconds: Long,
    )

    @JvmRecord
    data class EvidenceWaitResult(val found: Boolean, val snapshot: EvidenceSnapshot)

    @JvmRecord
    data class LookupOutcome(
        val found: Boolean,
        val spotId: String,
        val objectGeneration: Long,
        val meshName: String,
        val nodeRid: String,
        val errorKind: String,
        val errorMessage: String,
    )
}
