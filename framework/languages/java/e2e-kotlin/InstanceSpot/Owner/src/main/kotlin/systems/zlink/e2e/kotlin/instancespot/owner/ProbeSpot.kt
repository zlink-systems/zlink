package systems.zlink.e2e.kotlin.instancespot.owner

import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicLong
import systems.zlink.e2e.kotlin.instancespot.shared.Contracts
import systems.zlink.framework.kotlin.ZLinkSuspendingInstanceSpot
import systems.zlink.framework.kotlin.addHandler
import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionStage
import systems.zlink.framework.spots.ZLinkInstanceSpotContext
import systems.zlink.framework.spots.ZLinkSpotPacketHandler
import systems.zlink.framework.spots.ZLinkSpotRequestHandler
import systems.zlink.framework.spots.ZLinkSpotClosingContext

class ProbeSpot(
    override val context: ZLinkInstanceSpotContext,
    private val evidence: EvidenceStore,
    private val gates: GateController,
) : ZLinkSuspendingInstanceSpot() {
    private val handlerSequence = AtomicLong()
    private val activeHandlers = AtomicInteger()

    init {
        evidence.record(
            "FACTORY", context.spotId(), generation = context.objectGeneration(), detail = "kotlin-constructor",
        )
    }

    override fun configure() {
        context.handlers().addHandler<ProbeSendHandler>()
        context.handlers().addHandler<CloseHandler>()
    }

    override suspend fun onInitializeSuspending() {
        evidence.record("INITIALIZE", context.spotId(), generation = context.objectGeneration(), detail = "ready")
    }

    override suspend fun onClosingSuspending(closing: ZLinkSpotClosingContext) {
        evidence.record(
            "CLOSING", context.spotId(), generation = context.objectGeneration(), detail = closing.reason().name,
        )
    }

    fun enter(operationId: String, payload: String): Int = activeHandlers.incrementAndGet().also {
        evidence.record(
            "HANDLER_ENTER", context.spotId(), operationId, payload,
            context.objectGeneration(), it, "request",
        )
    }

    fun commit(operationId: String, payload: String) {
        val active = activeHandlers.decrementAndGet()
        evidence.record(
            "HANDLER_COMMIT", context.spotId(), operationId, payload,
            context.objectGeneration(), active, "domain-commit",
        )
    }

    fun awaitPayload(payload: String) = gates.awaitPayload(payload)
    fun awaitGate(gateId: String) = gates.await(gateId)
    fun nextSequence() = handlerSequence.incrementAndGet()
    fun evidence() = evidence
}

class ProbeRequestHandler : ZLinkSpotRequestHandler<ProbeSpot, Contracts.InstanceRequest, Contracts.InstanceReply> {
    override fun handle(
        spot: ProbeSpot,
        request: Contracts.InstanceRequest,
    ): CompletionStage<Contracts.InstanceReply> {
        spot.enter(request.operationId, request.payload)
        return spot.awaitPayload(request.payload).thenApply {
            Contracts.InstanceReply(
                spot.context.spotId(), request.operationId, request.payload,
                spot.evidence().snapshot().rid, spot.evidence().snapshot().lifecycleId,
                spot.context.objectGeneration(), spot.nextSequence(),
            ).also { spot.commit(request.operationId, request.payload) }
        }
    }
}

class ProbeSendHandler : ZLinkSpotPacketHandler<ProbeSpot, Contracts.InstanceSend> {
    override fun handle(spot: ProbeSpot, message: Contracts.InstanceSend): CompletionStage<Void> =
        spot.awaitPayload(message.payload).thenRun {
            spot.evidence().record(
            "SEND_HANDLER", spot.context.spotId(), message.operationId, message.payload,
            spot.context.objectGeneration(), detail = "one-way",
            )
        }
}

class CloseHandler : ZLinkSpotPacketHandler<ProbeSpot, Contracts.CloseRequest> {
    override fun handle(spot: ProbeSpot, message: Contracts.CloseRequest): CompletionStage<Void> =
        spot.awaitGate(message.gateId).thenCompose {
            spot.context.close().thenAccept { closed ->
                spot.evidence().record(
            "CLOSE_RESULT", spot.context.spotId(), message.operationId,
            generation = spot.context.objectGeneration(), detail = closed.toString(),
                )
            }
        }
}
