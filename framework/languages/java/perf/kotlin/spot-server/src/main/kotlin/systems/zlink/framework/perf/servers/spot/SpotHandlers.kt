package systems.zlink.framework.perf.servers.spot

import java.time.Duration
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.future.await
import systems.zlink.framework.kotlin.ZLinkSuspendingSpotPacketHandler
import systems.zlink.framework.kotlin.ZLinkSuspendingSpotRequestHandler
import systems.zlink.framework.kotlin.kotlin
import systems.zlink.framework.kotlin.requestToChannel
import systems.zlink.framework.kotlin.awaitReply
import systems.zlink.framework.kotlin.yieldReply
import systems.zlink.framework.perf.Evidence
import systems.zlink.framework.perf.Measurement
import systems.zlink.framework.perf.PayloadPattern
import systems.zlink.framework.perf.PerfClock
import systems.zlink.framework.perf.PerfDriveReply
import systems.zlink.framework.perf.PerfDriveRequest
import systems.zlink.framework.perf.PerfEchoReply
import systems.zlink.framework.perf.PerfEchoRequest
import systems.zlink.framework.perf.PerfValidationException
import systems.zlink.framework.perf.RoleConfig
import systems.zlink.framework.perf.ScenarioMetrics
import systems.zlink.framework.perf.SendSendCorrelation
import systems.zlink.framework.perf.WorkerObservation
import systems.zlink.framework.perf.DecimalText
import systems.zlink.framework.perf.servers.spot.PerfSpot
import systems.zlink.framework.spots.ZLinkWorkerCancellation
import systems.zlink.framework.spots.ZLinkSpotContext

class KotlinPerfEchoSpot(context: ZLinkSpotContext) : PerfSpot(context) {
    init { context.handlers().addHandler(KotlinPerfEchoRequestHandler::class.java) }
}

class KotlinPerfEchoRequestHandler(private val measurement: Measurement, private val metrics: ScenarioMetrics) :
    ZLinkSuspendingSpotRequestHandler<KotlinPerfEchoSpot, PerfEchoRequest, PerfEchoReply> {
    override suspend fun handle(spot: KotlinPerfEchoSpot, request: PerfEchoRequest): PerfEchoReply {
        val received = PerfClock.now()
        measurement.handlerEnter()
        try {
            measurement.validateRequest(request)
            if (request.phase() == "measured") {
                metrics.count("spot.applicationHandlerEntries")
            }
            val reply = PayloadPattern.reply(request, received)
            measurement.recordReply(request)
            if (measurement.phase() == "setup") {
                measurement.setupEvidence(listOf(Evidence.of("typedProbeReply", "Kotlin suspending Spot request handler", request.correlationId())))
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

class KotlinChannelToSpotSendSpot(context: ZLinkSpotContext) : PerfSpot(context) {
    init { context.handlers().addHandler(KotlinChannelToSpotSendHandler::class.java) }
}

class KotlinChannelToSpotSendHandler(private val measurement: Measurement) :
    ZLinkSuspendingSpotPacketHandler<KotlinChannelToSpotSendSpot, PerfEchoRequest> {
    override suspend fun handle(spot: KotlinChannelToSpotSendSpot, message: PerfEchoRequest) {
        val received = PerfClock.now()
        measurement.handlerEnter()
        try {
            val returnChannel = message.returnChannel()
            if (returnChannel.isNullOrEmpty()) throw PerfValidationException("IdentityMismatch", "No return Channel in the request.")
            measurement.validateRequest(message, returnChannel, null)
            val reply = PayloadPattern.reply(message, received)
            measurement.recordApplicationCall(message, "send")
            spot.context().outbound().kotlin().sendToChannel(returnChannel, reply).await()
            if (measurement.phase() == "setup") {
                measurement.setupEvidence(listOf(Evidence.of("typedProbeReply", "Kotlin Spot outbound sendToChannel(...).await()", message.correlationId())))
            }
        } catch (error: RuntimeException) {
            measurement.recordDiagnostic(error)
            throw error
        } finally {
            measurement.handlerExit()
        }
    }
}

class S2sRemoteRequestSpot(context: ZLinkSpotContext) : PerfSpot(context) {
    init { context.handlers().addHandler(S2sRemoteRequestDriveHandler::class.java) }
}

class S2sRemoteRequestDriveHandler(
    private val config: RoleConfig,
    private val measurement: Measurement,
    private val metrics: ScenarioMetrics,
) : ZLinkSuspendingSpotRequestHandler<S2sRemoteRequestSpot, PerfDriveRequest, PerfDriveReply> {
    override suspend fun handle(spot: S2sRemoteRequestSpot, drive: PerfDriveRequest): PerfDriveReply {
        measurement.handlerEnter()
        try {
            val request = drive.echo()
            measurement.validateRequest(request)
            if (request.phase() == "measured") metrics.count("spot.applicationHandlerEntries")
            val probe = measurement.phase() == "setup"
            val started = if (probe) PerfClock.now() else measurement.beginOperation()
            if (started < 0) return PerfDriveReply(false, null)
            val reply = try {
                val sent = request.withSentTicks(started)
                val call = spot.context().outbound().requestToChannel(config.channelName(), sent)
                    .timeout(Duration.ofMillis(config.workload().requestTimeoutMs().toLong()))
                if (config.terminal() == "yield") metrics.count("spot.applicationYieldCalls")
                val echoed = if (config.terminal() == "yield") call.yieldReply<PerfEchoReply>() else call.awaitReply<PerfEchoReply>()
                PayloadPattern.validateIdentity(sent, echoed)
                measurement.pattern().validate(echoed.payload())
                echoed
            } catch (error: Throwable) {
                if (probe) throw error
                measurement.completeOperation(started, error)
                if (error is CancellationException || error !is Exception) throw error
                return PerfDriveReply(true, null)
            }
            if (probe) {
                measurement.setupEvidence(listOf(Evidence.of("typedProbeReply", "Kotlin Spot outbound awaitReply/yieldReply", request.correlationId())))
                return PerfDriveReply(true, reply)
            }
            measurement.completeOperation(started)
            return PerfDriveReply(true, reply)
        } finally {
            measurement.handlerExit()
        }
    }

}

class S2sSendSendSpot(context: ZLinkSpotContext) : PerfSpot(context) {
    init {
        context.handlers().addHandler(S2sSendDriveHandler::class.java)
        context.handlers().addHandler(S2sSendReturnHandler::class.java)
    }
}

class S2sSendDriveHandler(
    private val config: RoleConfig,
    private val measurement: Measurement,
    private val metrics: ScenarioMetrics,
    private val correlations: SendSendCorrelation,
) : ZLinkSuspendingSpotRequestHandler<S2sSendSendSpot, PerfDriveRequest, PerfDriveReply> {
    override suspend fun handle(spot: S2sSendSendSpot, drive: PerfDriveRequest): PerfDriveReply {
        measurement.handlerEnter()
        try {
            val request = drive.echo()
            val returnSpotId = request.returnSpotId()
            if (returnSpotId.isNullOrEmpty()) throw PerfValidationException("IdentityMismatch", "No return SpotId in the request.")
            measurement.validateRequest(request, null, returnSpotId)
            if (request.phase() == "measured") metrics.count("spot.applicationHandlerEntries")
            val probe = measurement.phase() == "setup"
            val started = if (probe) PerfClock.now() else measurement.beginOperation("send")
            if (started < 0) return PerfDriveReply(false, null)
            val (sent, entry) = try {
                val sent = request.withSentTicks(started)
                sent to correlations.register(sent, started)
            } catch (error: Throwable) {
                if (!probe) measurement.completeOperation(started, error)
                throw error
            }
            try {
                spot.context().outbound().kotlin().sendToChannel(config.channelName(), sent).await()
                correlations.firstSendEnded(entry, null)
            } catch (error: Throwable) {
                correlations.firstSendEnded(entry, error)
                if (error is CancellationException || error !is Exception) throw error
            }
            return PerfDriveReply(true, null)
        } finally {
            measurement.handlerExit()
        }
    }
}

class S2sSendReturnHandler(private val correlations: SendSendCorrelation) :
    ZLinkSuspendingSpotPacketHandler<S2sSendSendSpot, PerfEchoReply> {
    override suspend fun handle(spot: S2sSendSendSpot, message: PerfEchoReply) { correlations.reply(message) }
}

class SpotWorkerOffloadSpot(context: ZLinkSpotContext) : PerfSpot(context) {
    init { context.handlers().addHandler(SpotWorkerOffloadHandler::class.java) }
}

class SpotWorkerOffloadHandler(
    private val config: RoleConfig,
    private val measurement: Measurement,
    private val metrics: ScenarioMetrics,
) : ZLinkSuspendingSpotRequestHandler<SpotWorkerOffloadSpot, PerfEchoRequest, PerfEchoReply> {
    override suspend fun handle(spot: SpotWorkerOffloadSpot, request: PerfEchoRequest): PerfEchoReply {
        val received = PerfClock.now()
        measurement.handlerEnter()
        try {
            measurement.validateRequest(request)
            if (request.phase() == "measured") metrics.count("spot.applicationHandlerEntries")
            val submitted = PerfClock.now()
            val call = spot.context().runCpuWorker({ worker -> xorShift32(config.worker().taskMillis(), worker) })
                .timeout(Duration.ofMillis(config.worker().workerTimeoutMs().toLong())).kotlin()
            if (config.terminal() == "yield") metrics.count("spot.applicationYieldCalls")
            val observation = if (config.terminal() == "yield") call.yield() else call.await()
            val resumed = PerfClock.now()
            recordWorker(request, observation, submitted, resumed)
            val reply = PayloadPattern.reply(request, received)
            measurement.recordReply(request)
            if (measurement.phase() == "setup" && !config.source()) {
                val observed = linkedMapOf<String, Any>(
                    "correlationId" to request.correlationId(),
                    "iterations" to observation.iterations(),
                    "checksum" to observation.checksum(),
                )
                measurement.setupEvidence(listOf(Evidence.of("typedProbeReply", "Kotlin suspending Spot handler -> runCpuWorker", observed)))
            }
            return reply
        } catch (error: Throwable) {
            measurement.recordDiagnostic(error)
            throw error
        } finally {
            measurement.handlerExit()
        }
    }

    private fun xorShift32(taskMillis: Int, cancellation: ZLinkWorkerCancellation): WorkerObservation {
        val started = PerfClock.now()
        val target = started + taskMillis * 1_000_000L
        var value = 0x12345678
        var iterations = 0L
        do {
            repeat(1024) {
                value = value xor (value shl 13)
                value = value xor (value ushr 17)
                value = value xor (value shl 5)
            }
            iterations += 1024
            cancellation.throwIfCancellationRequested()
        } while (PerfClock.now() < target)
        return WorkerObservation(DecimalText.of(started), DecimalText.of(PerfClock.now()), PerfClock.DOMAIN,
            DecimalText.of(iterations), Integer.toUnsignedLong(value))
    }

    private fun recordWorker(request: PerfEchoRequest, observation: WorkerObservation, submitted: Long, resumed: Long) {
        if (request.phase() != "measured") return
        if (PerfClock.DOMAIN != observation.clockDomainId() || DecimalText.u64(observation.iterations()) == 0L) {
            throw PerfValidationException("SchemaMismatch", "The worker observation is not from this clock domain or is empty.")
        }
        val started = DecimalText.i64(observation.startedTicks())
        val completed = DecimalText.i64(observation.endedTicks())
        metrics.record("workerCallLatencyMs", submitted, resumed)
        metrics.record("workerSubmitToStartMs", submitted, started, resumed)
        metrics.record("workerTaskLatencyMs", started, completed, resumed)
        metrics.record("workerResultToContinuationMs", completed, resumed, resumed)
    }
}
