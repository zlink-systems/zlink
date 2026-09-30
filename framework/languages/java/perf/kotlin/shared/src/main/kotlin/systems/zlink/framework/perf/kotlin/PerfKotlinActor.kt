package systems.zlink.framework.perf.kotlin

import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.actors.ZLinkActorContext
import systems.zlink.framework.channels.ZLinkRouteClient
import systems.zlink.framework.configuration.ZLinkMeshObjectServerBuilder
import systems.zlink.framework.handlers.ZLinkHandlerGroup
import systems.zlink.framework.kotlin.ZLinkSuspendingActor
import systems.zlink.framework.kotlin.ZLinkSuspendingActorFactory
import systems.zlink.framework.kotlin.ZLinkSuspendingEntrySpotActorRequestHandler
import systems.zlink.framework.kotlin.ZLinkSuspendingEntrySpotActorSendHandler
import systems.zlink.framework.kotlin.kotlin
import systems.zlink.framework.perf.Evidence
import systems.zlink.framework.perf.Measurement
import systems.zlink.framework.perf.PayloadPattern
import systems.zlink.framework.perf.PerfActorType
import systems.zlink.framework.perf.PerfClock
import systems.zlink.framework.perf.PerfEchoReply
import systems.zlink.framework.perf.PerfEchoRequest
import systems.zlink.framework.perf.RoleConfig

class PerfKotlinEntrySpot(
    private val entryContext: systems.zlink.framework.spots.ZLinkEntrySpotContext,
    private val config: RoleConfig,
) : systems.zlink.framework.spots.ZLinkEntrySpot<PerfKotlinActor> {
    init {
        if (config.mode() == "send-send") entryContext.handlers().addHandler(PerfKotlinActorSendHandler::class.java)
        else entryContext.handlers().addHandler(PerfKotlinActorRequestHandler::class.java)
    }

    override fun context(): systems.zlink.framework.spots.ZLinkEntrySpotContext =
        entryContext

    override fun onJoinedActor(actor: PerfKotlinActor) = java.util.concurrent.CompletableFuture.completedFuture<Void>(null)
    override fun onLeaveActor(actor: PerfKotlinActor) = java.util.concurrent.CompletableFuture.completedFuture<Void>(null)
}

class PerfKotlinActor(override val context: ZLinkActorContext) : ZLinkSuspendingActor() {
    override suspend fun onJoinCompletedSuspending(completion: systems.zlink.framework.actors.ZLinkActorJoinCompletion) {}
}

class PerfKotlinActorFactory : ZLinkSuspendingActorFactory() {
    override suspend fun createActor(context: ZLinkActorContext) = PerfKotlinActor(context)
}

class PerfKotlinActorMarker

@ZLinkHandlerGroup("perf-kotlin-actor-request")
class PerfKotlinActorRequestHandler(private val measurement: Measurement) :
    ZLinkSuspendingEntrySpotActorRequestHandler<PerfKotlinEntrySpot, PerfKotlinActor, PerfEchoRequest, PerfEchoReply> {
    override suspend fun handle(
        entrySpot: PerfKotlinEntrySpot,
        actor: PerfKotlinActor,
        context: ZLinkMessageContext,
        request: PerfEchoRequest,
    ): PerfEchoReply {
        val received = PerfClock.now()
        measurement.handlerEnter()
        try {
            measurement.validateRequest(request)
            val reply = PayloadPattern.reply(request, received)
            measurement.recordReply(request)
            if (measurement.phase() == "setup") {
                measurement.setupEvidence(listOf(Evidence.of("typedProbeReply", "Kotlin suspending Actor request handler", request.correlationId())))
            }
            return reply
        } catch (error: RuntimeException) {
            measurement.recordDiagnostic(error)
            throw error
        } finally {
            measurement.handlerExit()
        }
    }
}

@ZLinkHandlerGroup("perf-kotlin-actor-send")
class PerfKotlinActorSendHandler(
    private val measurement: Measurement,
    private val config: RoleConfig,
    private val route: ZLinkRouteClient,
) : ZLinkSuspendingEntrySpotActorSendHandler<PerfKotlinEntrySpot, PerfKotlinActor, PerfEchoRequest> {
    override suspend fun handle(
        entrySpot: PerfKotlinEntrySpot,
        actor: PerfKotlinActor,
        context: ZLinkMessageContext,
        message: PerfEchoRequest,
    ) {
        val received = PerfClock.now()
        measurement.handlerEnter()
        try {
            measurement.validateRequest(message, config.channelName(), null)
            val reply = PayloadPattern.reply(message, received)
            measurement.recordApplicationCall(message, "send")
            route.kotlin().sendToChannel(message.returnChannel(), reply).await()
            if (measurement.phase() == "setup") {
                measurement.setupEvidence(listOf(Evidence.of("typedProbeReply", "Kotlin Channel send wrapper await()", message.correlationId())))
            }
        } catch (error: RuntimeException) {
            measurement.recordDiagnostic(error)
            throw error
        } finally {
            measurement.handlerExit()
        }
    }
}

fun addPerfKotlinActors(objects: ZLinkMeshObjectServerBuilder) {
    objects.addEntrySpot(PerfKotlinEntrySpot::class.java)
        .addActorFactory(
            PerfActorType.NAME,
            PerfKotlinActor::class.java,
            PerfKotlinActorFactory::class.java,
        ) { it.disableRelocation() }
}
