package systems.zlink.e2e.kotlin.spotactortransfer.actor

import java.time.Duration
import java.nio.ByteBuffer
import java.nio.charset.StandardCharsets
import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionStage
import java.util.concurrent.ConcurrentHashMap
import kotlinx.coroutines.future.await
import systems.zlink.e2e.spotactortransfer.shared.Contracts
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.actors.ZLinkActor
import systems.zlink.framework.actors.ZLinkActorContext
import systems.zlink.framework.actors.ZLinkActorJoinCompletion
import systems.zlink.framework.actors.ZLinkActorRelocationAdapter
import systems.zlink.framework.actors.ZLinkRelocationCancellation
import systems.zlink.framework.kotlin.ZLinkSuspendingActor
import systems.zlink.framework.kotlin.ZLinkSuspendingActorFactory
import systems.zlink.framework.kotlin.ZLinkSuspendingEntrySpot
import systems.zlink.framework.kotlin.ZLinkSuspendingEntrySpotActorRequestHandler
import systems.zlink.framework.kotlin.ZLinkSuspendingSpot
import systems.zlink.framework.kotlin.ZLinkSuspendingSpotActorSendHandler
import systems.zlink.framework.kotlin.ZLinkSuspendingSpotActorRequestHandler
import systems.zlink.framework.kotlin.ZLinkSuspendingSession
import systems.zlink.framework.kotlin.addHandler
import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.framework.spots.ZLinkEntrySpotContext
import systems.zlink.framework.spots.ZLinkSpotActorJoinResult
import systems.zlink.framework.spots.ZLinkActorCreateResponse
import systems.zlink.framework.spots.ZLinkSpotContext
import systems.zlink.framework.spots.ZLinkSpotCreateResponse
import systems.zlink.framework.streams.ZLinkSessionContext
import systems.zlink.framework.streams.ZLinkSessionDispatchContext
import systems.zlink.framework.streams.ZLinkSessionPacketDispatcher
import systems.zlink.framework.streams.ZLinkStreamError
import systems.zlink.framework.kotlin.ZLinkSuspendingTypedSessionPacketHandler

class TransferActor(
    override val context: ZLinkActorContext,
    private val evidence: EvidenceStore,
) : ZLinkSuspendingActor() {
    var actorType: String = Contracts.STATEFUL
    var stateVersion: Int = 0

    fun actorId(): String = context.actorId()

    override suspend fun onJoinCompletedSuspending(completion: ZLinkActorJoinCompletion) {
        when (completion) {
            is ZLinkActorJoinCompletion.Accepted ->
                evidence.add("transfer", actorId(), "commit_ack", completion.actor().nodeRid().toString())
            is ZLinkActorJoinCompletion.Rejected ->
                evidence.add("transfer", actorId(), "join_rejected", "")
            is ZLinkActorJoinCompletion.Failed ->
                evidence.add("transfer", actorId(), "join_failed", completion.kind().name)
        }
    }
}

class TransferActorFactory(
    private val evidence: EvidenceStore,
) : ZLinkSuspendingActorFactory() {
    override suspend fun createActor(context: ZLinkActorContext): ZLinkActor {
        val actorId = context.actorId()
        if (evidence.nodeRid == "actor-b" && actorId.startsWith("actor-no-adapter-")) {
            evidence.add("transfer", actorId, "transfer_in_empty_default", "actor-factory")
        }
        return TransferActor(context, evidence)
    }
}

class TransferActorAdapter(
    private val evidence: EvidenceStore,
    private val gates: GateStore,
) : ZLinkActorRelocationAdapter<TransferActor> {
    override fun capture(
        actor: TransferActor,
        cancellation: ZLinkRelocationCancellation,
    ): CompletionStage<ByteArray> {
        if (actor.actorType == Contracts.FAIL_OUT) {
            evidence.add("ST-C3", actor.actorId(), "transfer_out_failed", actor.stateVersion.toString())
            return CompletableFuture.failedFuture(IllegalStateException("injected transfer out failure"))
        }
        if (actor.actorType == Contracts.EMPTY_STATE) {
            evidence.add("transfer", actor.actorId(), "transfer_out_empty", "custom-adapter")
            return CompletableFuture.completedFuture(byteArrayOf())
        }
        evidence.add("transfer", actor.actorId(), "transfer_out", actor.stateVersion.toString())
        if (actor.actorId().startsWith("actor-source-down-before-commit-")) {
            evidence.add("ST-C1", actor.actorId(), "before_commit_gate", actor.stateVersion.toString())
            return gates.waitFor(actor.actorId()).thenApply { transferState(actor) }
        }
        return CompletableFuture.completedFuture(transferState(actor))
    }

    override fun restore(
        actor: TransferActor,
        state: ByteArray,
        cancellation: ZLinkRelocationCancellation,
    ): CompletionStage<Void> {
        val actorId = actor.actorId()
        if (state.isEmpty()) {
            evidence.add("transfer", actorId, "transfer_in_empty", "custom-adapter")
            actor.actorType = Contracts.EMPTY_STATE
            return CompletableFuture.completedFuture(null)
        }
        val payload = ByteBuffer.wrap(state)
        val stateVersion = payload.int
        val actorTypeLength = payload.int
        if (actorTypeLength < 0 || actorTypeLength > payload.remaining()) {
            return CompletableFuture.failedFuture(
                IllegalArgumentException("invalid Actor relocation state"),
            )
        }
        val actorType = ByteArray(actorTypeLength)
        payload.get(actorType)
        if (actorId.startsWith("actor-fail-transfer-in-")) {
            evidence.add("ST-C3", actorId, "transfer_in_failed", stateVersion.toString())
            return CompletableFuture.failedFuture(IllegalStateException("injected transfer in failure"))
        }
        actor.actorType = String(actorType, StandardCharsets.UTF_8)
        actor.stateVersion = stateVersion
        evidence.add("transfer", actorId, "transfer_in", actor.stateVersion.toString())
        return CompletableFuture.completedFuture(null)
    }

    private fun transferState(actor: TransferActor): ByteArray {
        val actorType = actor.actorType.toByteArray(StandardCharsets.UTF_8)
        return ByteBuffer.allocate(Int.SIZE_BYTES * 2 + actorType.size)
            .putInt(actor.stateVersion)
            .putInt(actorType.size)
            .put(actorType)
            .array()
    }
}

class TransferEntrySpot(
    private val entryContext: ZLinkEntrySpotContext,
    private val evidence: EvidenceStore,
    private val domainState: DomainStateStore,
) : ZLinkSuspendingEntrySpot<TransferActor>() {
    override val context: ZLinkEntrySpotContext = entryContext

    override fun configure() {
        entryContext.handlers().addHandler<JoinTargetHandler>()
        entryContext.handlers().addHandler<EntryProbeHandler>()
        entryContext.handlers().addHandler<EntryBoundPushHandler>()
    }

    override suspend fun onCreateActorSuspending(
        actor: TransferActor,
        createRequest: ZLinkMessage,
    ): ZLinkActorCreateResponse {
        if (!createRequest.isEmpty) {
            val request = createRequest.decode(Contracts.ActorCreateReq::class.java)
            actor.actorType = request.actorType()
            actor.stateVersion = request.stateVersion()
            if (actor.actorType == Contracts.EMPTY_STATE) {
                domainState.save(actor.actorId(), actor.stateVersion)
            }
        }
        evidence.add("create", actor.actorId(), "create", "${actor.actorType}:${actor.stateVersion}")
        return ZLinkActorCreateResponse.accept()
    }

    override suspend fun onJoinedActorSuspending(actor: TransferActor) {
        evidence.add("local", actor.actorId(), "entry_joined", actor.stateVersion.toString())
    }

    override suspend fun onLeaveActorSuspending(actor: TransferActor) {
        if (actor.actorType == Contracts.NO_ADAPTER) {
            evidence.add("transfer", actor.actorId(), "transfer_out_empty_default", "no-adapter")
        }
        if (actor.actorType == Contracts.FAIL_LEAVE) {
            evidence.add("ST-C3", actor.actorId(), "leave_failed", actor.stateVersion.toString())
            error("injected source leave failure")
        }
        evidence.add("transfer", actor.actorId(), "leave", actor.stateVersion.toString())
    }
}

class TransferUserSpot(
    private val spotContext: ZLinkSpotContext,
    private val evidence: EvidenceStore,
    private val gates: GateStore,
    private val domainState: DomainStateStore,
) : ZLinkSuspendingSpot<TransferActor>() {
    private val scenarios = ConcurrentHashMap<String, String>()
    private var mode = "accept"

    override val context: ZLinkSpotContext = spotContext

    override fun configure() {
        spotContext.handlers().addHandler<UserJoinTargetHandler>()
        spotContext.handlers().addHandler<ProbeHandler>()
        spotContext.handlers().addHandler<BoundPushHandler>()
        spotContext.handlers().addHandler<MessageFollowSendHandler>()
    }

    override suspend fun onCreateSuspending(request: ZLinkMessage): ZLinkSpotCreateResponse {
        if (!request.isEmpty) {
            mode = request.decode(Contracts.CreateSpotReq::class.java).mode()
                ?.takeIf(String::isNotBlank) ?: "accept"
        }
        evidence.add("create_spot", spotContext.spotId(), "spot_created", mode)
        return ZLinkSpotCreateResponse.accept()
    }

    override suspend fun onActorJoinSuspending(
        actorId: String,
        request: ZLinkMessage,
    ): ZLinkSpotActorJoinResult {
        val join = request.decode(Contracts.JoinTargetReq::class.java)
        scenarios[actorId] = join.scenario()
        evidence.add(
            join.scenario(),
            actorId,
            "admission",
            "spot=${spotContext.spotId()};mode=$mode;input=actor-id-only",
        )
        val reject = mode == "reject" || join.expectedMode() == "reject"
        val response = Contracts.JoinTargetRes(
            join.scenario(), actorId, !reject, "", spotContext.spotId(), 0,
        )
        return if (reject) ZLinkSpotActorJoinResult.reject(response)
        else ZLinkSpotActorJoinResult.accept(response)
    }

    override suspend fun onJoinedActorSuspending(actor: TransferActor) {
        val scenario = scenarios[actor.actorId()] ?: "transfer"
        if (mode == "delay-joined") {
            evidence.add(scenario, actor.actorId(), "joined_wait", spotContext.spotId())
            gates.waitFor(spotContext.spotId()).await()
            evidence.add(scenario, actor.actorId(), "joined_released", spotContext.spotId())
        }
        if (mode == "fail-joined") {
            evidence.add(scenario, actor.actorId(), "joined_failed", spotContext.spotId())
            error("injected joined failure")
        }
        evidence.add("transfer", actor.actorId(), "joined", "${spotContext.spotId()}:${actor.stateVersion}")
        if (actor.actorType == Contracts.EMPTY_STATE) {
            actor.stateVersion = domainState.load(actor.actorId())
            evidence.add("transfer", actor.actorId(), "domain_state_loaded", actor.stateVersion.toString())
        }
    }

    override suspend fun onLeaveActorSuspending(actor: TransferActor) {
        evidence.add("transfer", actor.actorId(), "target_leave", spotContext.spotId())
    }
}

class JoinTargetHandler(
    private val evidence: EvidenceStore,
) : ZLinkSuspendingEntrySpotActorRequestHandler<
    TransferEntrySpot,
    TransferActor,
    Contracts.JoinTargetReq,
    Contracts.JoinTargetRes
> {
    override suspend fun handle(
        entrySpot: TransferEntrySpot,
        actor: TransferActor,
        context: ZLinkMessageContext,
        request: Contracts.JoinTargetReq,
    ): Contracts.JoinTargetRes {
        return joinTarget(actor, request, evidence)
    }
}

class UserJoinTargetHandler(
    private val evidence: EvidenceStore,
) : ZLinkSuspendingSpotActorRequestHandler<
    TransferUserSpot,
    TransferActor,
    Contracts.JoinTargetReq,
    Contracts.JoinTargetRes
> {
    override suspend fun handle(
        spot: TransferUserSpot,
        actor: TransferActor,
        context: ZLinkMessageContext,
        request: Contracts.JoinTargetReq,
    ): Contracts.JoinTargetRes = joinTarget(actor, request, evidence)
}

private suspend fun joinTarget(
    actor: TransferActor,
    request: Contracts.JoinTargetReq,
    evidence: EvidenceStore,
): Contracts.JoinTargetRes {
    actor.context()
        .joinSpot(request.targetSpotRid(), request)
        .timeout(Duration.ofSeconds(10))
        .defer()
    evidence.add(request.scenario(), actor.actorId(), "join_deferred", request.targetSpotRid())
    return Contracts.JoinTargetRes(
        request.scenario(), actor.actorId(), true, evidence.nodeRid,
        request.targetSpotRid(), actor.stateVersion,
    )
}

class EntryProbeHandler(
    private val evidence: EvidenceStore,
) : ZLinkSuspendingEntrySpotActorRequestHandler<
    TransferEntrySpot,
    TransferActor,
    Contracts.ProbeReq,
    Contracts.ProbeRes
> {
    override suspend fun handle(
        entrySpot: TransferEntrySpot,
        actor: TransferActor,
        context: ZLinkMessageContext,
        request: Contracts.ProbeReq,
    ): Contracts.ProbeRes {
        evidence.add(request.scenario(), actor.actorId(), "entry_packet_handler", request.marker())
        return Contracts.ProbeRes(
            request.scenario(), actor.actorId(), entrySpot.context().spotId(),
            evidence.nodeRid, actor.stateVersion, request.marker(),
        )
    }
}

class ProbeHandler(
    private val evidence: EvidenceStore,
) : ZLinkSuspendingSpotActorRequestHandler<
    TransferUserSpot,
    TransferActor,
    Contracts.ProbeReq,
    Contracts.ProbeRes
> {
    override suspend fun handle(
        spot: TransferUserSpot,
        actor: TransferActor,
        context: ZLinkMessageContext,
        request: Contracts.ProbeReq,
    ): Contracts.ProbeRes {
        evidence.add(request.scenario(), actor.actorId(), "packet_handler", request.marker())
        return Contracts.ProbeRes(
            request.scenario(), actor.actorId(), spot.context().spotId(),
            evidence.nodeRid, actor.stateVersion, request.marker(),
        )
    }
}

class MessageFollowSendHandler(
    private val evidence: EvidenceStore,
) : ZLinkSuspendingSpotActorSendHandler<
    TransferUserSpot,
    TransferActor,
    Contracts.MessageFollowSendReq
> {
    override suspend fun handle(
        spot: TransferUserSpot,
        actor: TransferActor,
        context: ZLinkMessageContext,
        message: Contracts.MessageFollowSendReq,
    ) {
        evidence.add(message.scenario(), actor.actorId(), "message_follow_send", message.marker())
    }
}

class TransferSession(
    private val sessionContext: ZLinkSessionContext,
    private val handlers: ZLinkSessionPacketDispatcher<ZLinkSessionContext>,
    private val evidence: EvidenceStore,
) : ZLinkSuspendingSession() {
    override fun context(): ZLinkSessionContext = sessionContext

    override suspend fun onConnectedSuspending() {
        evidence.add("session", sessionContext.sessionId(), "connected", "")
    }

    override suspend fun onDisconnectedSuspending() {
        evidence.add("session", sessionContext.sessionId(), "disconnected", "")
    }

    override suspend fun onErrorSuspending(error: ZLinkStreamError) {
        evidence.add("session", sessionContext.sessionId(), "error", error.toString())
    }

    override suspend fun onDispatchSuspending(
        dispatch: ZLinkSessionDispatchContext,
        payload: ZLinkMessage,
    ) {
        if (handlers.tryHandle(sessionContext, dispatch, payload).await()) return
        val actor = sessionContext.actors().bound().singleOrNull()
            ?: sessionContext.actors().find(dispatch.metadata()["actor-id"])
                .orElseThrow { IllegalStateException("actor is not bound") }
        actor.relay(dispatch, payload).await()
    }
}

class BindSessionHandler(
    private val actors: systems.zlink.framework.actors.ZLinkActorManager,
    private val evidence: EvidenceStore,
) : ZLinkSuspendingTypedSessionPacketHandler<ZLinkSessionContext, Contracts.BindSessionReq> {
    override fun packetName(): String = "BindSessionReq"
    override fun messageType(): Class<Contracts.BindSessionReq> = Contracts.BindSessionReq::class.java

    override suspend fun handle(
        context: ZLinkSessionContext,
        dispatch: ZLinkSessionDispatchContext,
        request: Contracts.BindSessionReq,
    ) {
        val actor = actors.find(request.actorId()).await()
            .orElseThrow { IllegalStateException("actor was not found: ${request.actorId()}") }
        val bound = context.actors().bind(actor).await()
        evidence.add(request.scenario(), request.actorId(), "session_bound", context.sessionId())
        context.client().reply(
            Contracts.BindSessionRes(
                request.scenario(), request.actorId(), bound.ref().nodeRid().toString(),
                bound.ref().objectGeneration(),
            ),
        ).submit()
    }
}

class EntryBoundPushHandler(
    private val evidence: EvidenceStore,
) : ZLinkSuspendingEntrySpotActorRequestHandler<
    TransferEntrySpot,
    TransferActor,
    Contracts.BoundPushReq,
    Contracts.BoundPushRes
> {
    override suspend fun handle(
        entrySpot: TransferEntrySpot,
        actor: TransferActor,
        context: ZLinkMessageContext,
        request: Contracts.BoundPushReq,
    ): Contracts.BoundPushRes = push(actor, entrySpot.context().spotId(), request, evidence)
}

class BoundPushHandler(
    private val evidence: EvidenceStore,
) : ZLinkSuspendingSpotActorRequestHandler<
    TransferUserSpot,
    TransferActor,
    Contracts.BoundPushReq,
    Contracts.BoundPushRes
> {
    override suspend fun handle(
        spot: TransferUserSpot,
        actor: TransferActor,
        context: ZLinkMessageContext,
        request: Contracts.BoundPushReq,
    ): Contracts.BoundPushRes = push(actor, spot.context().spotId(), request, evidence)
}

private fun push(
    actor: TransferActor,
    spotRid: String,
    request: Contracts.BoundPushReq,
    evidence: EvidenceStore,
): Contracts.BoundPushRes {
    actor.context().boundSession()
        .send(
            Contracts.BoundPushNotify(
                request.scenario(), actor.actorId(), spotRid, evidence.nodeRid,
                request.marker(), actor.stateVersion,
            ),
        )
        .submit()
    evidence.add(request.scenario(), actor.actorId(), "bound_push", request.marker())
    return Contracts.BoundPushRes(
        request.scenario(), actor.actorId(), spotRid, evidence.nodeRid,
        request.marker(), actor.stateVersion,
    )
}
