package systems.zlink.framework.perf.servers.channel

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.future.await
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.channels.ZLinkRouteClient
import systems.zlink.framework.configuration.ZLinkFrameworkOptions
import systems.zlink.framework.handlers.ZLinkHandlerGroup
import systems.zlink.framework.kotlin.ZLinkSuspendingRequestHandler
import systems.zlink.framework.kotlin.ZLinkSuspendingSendHandler
import systems.zlink.framework.kotlin.addHandlersFromPackageOf
import systems.zlink.framework.kotlin.kotlin
import systems.zlink.framework.kotlin.sendToSpot
import systems.zlink.framework.kotlin.useCoroutineHandlers
import systems.zlink.framework.perf.Evidence
import systems.zlink.framework.perf.Measurement
import systems.zlink.framework.perf.PerfClock
import systems.zlink.framework.perf.PerfEchoReply
import systems.zlink.framework.perf.PerfEchoRequest
import systems.zlink.framework.perf.PerfValidationException
import systems.zlink.framework.perf.PayloadPattern
import systems.zlink.framework.perf.RoleConfig
import systems.zlink.framework.perf.SendSendCorrelation

internal const val BASELINE_GROUP = "perf-kotlin-channel-baseline"
internal const val RETURN_GROUP = "perf-kotlin-channel-return"
internal const val SPOT_RETURN_GROUP = "perf-kotlin-channel-spot-return"

internal fun ZLinkFrameworkOptions.enableKotlinPerfHandlers() {
    addHandlersFromPackageOf<ChannelHandlerMarker>()
    useCoroutineHandlers(Dispatchers.IO)
}

internal class ChannelHandlerMarker

@ZLinkHandlerGroup(BASELINE_GROUP)
class KotlinChannelEchoHandler(private val measurement: Measurement) :
    ZLinkSuspendingRequestHandler<PerfEchoRequest, PerfEchoReply> {
    override suspend fun handle(request: PerfEchoRequest, context: ZLinkMessageContext): PerfEchoReply {
        val received = PerfClock.now()
        measurement.handlerEnter()
        try {
            measurement.validateRequest(request)
            val reply = PayloadPattern.reply(request, received)
            measurement.recordReply(request)
            if (measurement.phase() == "setup") {
                measurement.setupEvidence(listOf(Evidence.of("typedProbeReply", "Kotlin suspending Channel request handler", request.correlationId())))
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

@ZLinkHandlerGroup(RETURN_GROUP)
class ChannelReturnHandler(private val correlations: SendSendCorrelation) :
    ZLinkSuspendingSendHandler<PerfEchoReply> {
    override suspend fun handle(message: PerfEchoReply, context: ZLinkMessageContext) {
        correlations.reply(message)
    }
}

@ZLinkHandlerGroup(SPOT_RETURN_GROUP)
class ChannelSpotReturnHandler(
    private val measurement: Measurement,
    private val spots: ZLinkRouteClient,
    private val config: RoleConfig,
) : ZLinkSuspendingSendHandler<PerfEchoRequest> {
    override suspend fun handle(message: PerfEchoRequest, context: ZLinkMessageContext) {
        val received = PerfClock.now()
        measurement.handlerEnter()
        try {
            if (message.clientId() < 0 || config.spotIds().isEmpty()) {
                throw PerfValidationException("IdentityMismatch", "The request has no source Spot in this cell.")
            }
            val returnSpotId = config.spotIds()[message.clientId() % config.spotIds().size]
            measurement.validateRequest(message, null, returnSpotId)
            val reply = PayloadPattern.reply(message, received)
            measurement.recordApplicationCall(message, "send")
            spots.kotlin().sendToSpot(returnSpotId, reply).await()
            if (measurement.phase() == "setup") {
                measurement.setupEvidence(listOf(Evidence.of("typedProbeReply", "Kotlin route sendToSpot(...).await()", message.correlationId())))
            }
        } catch (error: RuntimeException) {
            measurement.recordDiagnostic(error)
            throw error
        } finally {
            measurement.handlerExit()
        }
    }
}
