package systems.zlink.e2e.kotlin.spotservice.multinode

import systems.zlink.e2e.kotlin.spotservice.Contracts
import kotlinx.coroutines.future.await
import systems.zlink.framework.actors.ZLinkActor
import systems.zlink.framework.actors.ZLinkActorContext
import systems.zlink.framework.kotlin.ZLinkSuspendingActorFactory
import systems.zlink.framework.kotlin.ZLinkSuspendingEntrySpot
import systems.zlink.framework.kotlin.ZLinkSuspendingEntrySpotActorRequestHandler
import systems.zlink.framework.kotlin.ZLinkSuspendingSpot
import systems.zlink.framework.kotlin.ZLinkSuspendingSpotPacketHandler
import systems.zlink.framework.kotlin.addHandler
import systems.zlink.framework.handlers.ZLinkSpotRequest
import systems.zlink.framework.handlers.ZLinkSpotActorRequest
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.framework.spots.ZLinkEntrySpotContext
import systems.zlink.framework.spots.ZLinkSpotActorJoinResult
import systems.zlink.framework.spots.ZLinkSpotContext
import systems.zlink.framework.spots.ZLinkSpotCreateResponse
import systems.zlink.framework.spots.ZLinkActorCreateResponse

class MultiNodeSpotA(
    override val context: ZLinkSpotContext,
    private val evidence: MultiNodeEvidenceStore,
) : ZLinkSuspendingSpot<MultiNodeActor>() {
    private var value = 0

    override fun configure() {
        context.handlers().addHandler<MultiNodeStateAHandler>()
        context.handlers().addHandler<MultiNodeStateCommandAHandler>()
    }

    fun add(delta: Int): Int {
        value += delta
        return value
    }

    override suspend fun onCreateSuspending(request: ZLinkMessage): ZLinkSpotCreateResponse =
        onCreateMultiNodeSpot(context, evidence, request)

    override suspend fun onInitializeSuspending() {
        evidence.add("multi-spot-initialize|node=${Contracts.MULTI_SPOT_NODE_A}|spot=${context.spotId()}")
    }

    override suspend fun onActorJoinSuspending(
        actorId: String,
        request: ZLinkMessage,
    ): ZLinkSpotActorJoinResult {
        return ZLinkSpotActorJoinResult.accept()
    }

    override suspend fun onJoinedActorSuspending(actor: MultiNodeActor) {
        evidence.add("spot-actor-joined|node=${Contracts.MULTI_SPOT_NODE_A}|spot=${context.spotId()}|actor=${actor.actorId()}")
    }

    override suspend fun onLeaveActorSuspending(actor: MultiNodeActor) {
    }
}

class MultiNodeSpotB(
    override val context: ZLinkSpotContext,
    private val evidence: MultiNodeEvidenceStore,
) : ZLinkSuspendingSpot<MultiNodeActor>() {
    private var value = 0

    override fun configure() {
        context.handlers().addHandler<MultiNodeStateBHandler>()
        context.handlers().addHandler<MultiNodeStateCommandBHandler>()
    }

    fun add(delta: Int): Int {
        value += delta
        return value
    }

    override suspend fun onCreateSuspending(request: ZLinkMessage): ZLinkSpotCreateResponse =
        onCreateMultiNodeSpot(context, evidence, request)

    override suspend fun onInitializeSuspending() {
        evidence.add("multi-spot-initialize|node=${Contracts.MULTI_SPOT_NODE_B}|spot=${context.spotId()}")
    }

    override suspend fun onActorJoinSuspending(
        actorId: String,
        request: ZLinkMessage,
    ): ZLinkSpotActorJoinResult {
        return ZLinkSpotActorJoinResult.accept()
    }

    override suspend fun onJoinedActorSuspending(actor: MultiNodeActor) {
        evidence.add("spot-actor-joined|node=${Contracts.MULTI_SPOT_NODE_B}|spot=${context.spotId()}|actor=${actor.actorId()}")
    }

    override suspend fun onLeaveActorSuspending(actor: MultiNodeActor) {
    }
}

private suspend fun onCreateMultiNodeSpot(
    context: ZLinkSpotContext,
    evidence: MultiNodeEvidenceStore,
    request: ZLinkMessage
): ZLinkSpotCreateResponse {
    evidence.add("spot-created|node=${context.nodeRid()}|spot=${context.spotId()}")
    if (request.isEmpty) {
        return ZLinkSpotCreateResponse.accept()
    }
    val command = request.decode(Contracts.SpotOnlyMeshReq::class.java)
    val reply = context.outbound()
        .requestToSpot(command.targetSpotRid, Contracts.MultiNodeStateReq("add", 7))
        .timeout(java.time.Duration.ofSeconds(5))
        .submit(Contracts.MultiNodeStateRes::class.java).await()
    context.outbound()
        .sendToSpot(command.targetSpotRid, Contracts.StateMsg("sm-f6-send-${command.marker}"))
        .submit()
    evidence.add(
        "spot-only-request|node=${context.nodeRid()}|source=${context.spotId()}" +
            "|target=${command.targetSpotRid}|value=${reply.value}|marker=${command.marker}"
    )
    return ZLinkSpotCreateResponse.accept(
        Contracts.SpotOnlyMeshRes(
            context.spotId(),
            command.targetSpotRid,
            reply.value
        )
    )
}

class MultiNodeStateAHandler(
    private val evidence: MultiNodeEvidenceStore
) {
    @ZLinkSpotRequest
    suspend fun handle(
        spot: MultiNodeSpotA,
        request: Contracts.MultiNodeStateReq
    ): Contracts.MultiNodeStateRes {
        val delta = if (request.operation == "add") request.delta else 0
        val value = spot.add(delta)
        evidence.add("multi-state-request|node=${Contracts.MULTI_SPOT_NODE_A}|spot=${spot.context().spotId()}|value=$value")
        return Contracts.MultiNodeStateRes(
            spot.context().spotId(),
            spot.context().nodeRid().toString(),
            value
        )
    }
}

class MultiNodeStateCommandAHandler(
    private val evidence: MultiNodeEvidenceStore
) : ZLinkSuspendingSpotPacketHandler<MultiNodeSpotA, Contracts.StateMsg> {
    override suspend fun handle(
        spot: MultiNodeSpotA,
        command: Contracts.StateMsg
    ) {
        evidence.add("spot-state-command|node=${Contracts.MULTI_SPOT_NODE_A}|spot=${spot.context().spotId()}|marker=${command.value}")
    }
}

class MultiNodeStateBHandler(
    private val evidence: MultiNodeEvidenceStore
) {
    @ZLinkSpotRequest
    suspend fun handle(
        spot: MultiNodeSpotB,
        request: Contracts.MultiNodeStateReq
    ): Contracts.MultiNodeStateRes {
        val delta = if (request.operation == "add") request.delta else 0
        val value = spot.add(delta)
        evidence.add("multi-state-request|node=${Contracts.MULTI_SPOT_NODE_B}|spot=${spot.context().spotId()}|value=$value")
        return Contracts.MultiNodeStateRes(
            spot.context().spotId(),
            spot.context().nodeRid().toString(),
            value
        )
    }
}

class MultiNodeStateCommandBHandler(
    private val evidence: MultiNodeEvidenceStore
) : ZLinkSuspendingSpotPacketHandler<MultiNodeSpotB, Contracts.StateMsg> {
    override suspend fun handle(
        spot: MultiNodeSpotB,
        command: Contracts.StateMsg
    ) {
        evidence.add("spot-state-command|node=${Contracts.MULTI_SPOT_NODE_B}|spot=${spot.context().spotId()}|marker=${command.value}")
    }
}

class MultiNodeActor(
    private val actorId: String,
    private val context: ZLinkActorContext
) : ZLinkActor {
    fun actorId(): String = actorId

    override fun context(): ZLinkActorContext = context
}

class MultiNodeActorFactory : ZLinkSuspendingActorFactory() {
    override suspend fun createActor(context: ZLinkActorContext): ZLinkActor =
        MultiNodeActor(context.actorId(), context)
}

class MultiNodeEntrySpot(
    override val context: ZLinkEntrySpotContext
) : ZLinkSuspendingEntrySpot<MultiNodeActor>() {
    override suspend fun onCreateActorSuspending(
        actor: MultiNodeActor,
        createRequest: ZLinkMessage,
    ): ZLinkActorCreateResponse = ZLinkActorCreateResponse.accept()

    override suspend fun onJoinedActorSuspending(actor: MultiNodeActor) {
    }

    override suspend fun onLeaveActorSuspending(actor: MultiNodeActor) {
    }
    override fun configure() {
        context.handlers().addHandler<MultiNodeSpotOnlyJoinHandler>()
    }
}

class MultiNodeSpotOnlyJoinHandler : ZLinkSuspendingEntrySpotActorRequestHandler<MultiNodeEntrySpot, MultiNodeActor, Contracts.SpotOnlyJoinReq, Contracts.SpotOnlyJoinRes> {
    @ZLinkSpotActorRequest(packetName = "SpotOnlyJoinReq")
    override suspend fun handle(
        spot: MultiNodeEntrySpot,
        actor: MultiNodeActor,
        context: ZLinkMessageContext,
        request: Contracts.SpotOnlyJoinReq,
    ): Contracts.SpotOnlyJoinRes {
        actor.context()
            .joinSpot(request.targetSpotRid, request)
            .defer()
        return Contracts.SpotOnlyJoinRes(true, actor.actorId())
    }
}
