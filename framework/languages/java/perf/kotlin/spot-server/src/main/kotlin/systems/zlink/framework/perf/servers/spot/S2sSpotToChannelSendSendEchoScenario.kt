package systems.zlink.framework.perf.servers.spot

import java.util.concurrent.CompletionStage
import java.util.concurrent.atomic.AtomicLongArray
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.future.await
import systems.zlink.framework.channels.ZLinkRouteClient
import systems.zlink.framework.kotlin.kotlin
import systems.zlink.framework.kotlin.requestToSpot
import systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime
import systems.zlink.framework.perf.Evidence
import systems.zlink.framework.perf.Measurement
import systems.zlink.framework.perf.ObjectsReadiness
import systems.zlink.framework.perf.PerfClock
import systems.zlink.framework.perf.PerfDriveReply
import systems.zlink.framework.perf.PerfDriveRequest
import systems.zlink.framework.perf.PerfValidationException
import systems.zlink.framework.perf.Polling
import systems.zlink.framework.perf.RoleConfig
import systems.zlink.framework.perf.ScenarioMetrics
import systems.zlink.framework.perf.SendSendCorrelation
import systems.zlink.framework.perf.kotlin.completionStage
import systems.zlink.framework.perf.kotlin.planStreamTargets
import systems.zlink.framework.perf.kotlin.runTerminalStreams
import systems.zlink.framework.spots.ZLinkSpotManager

class S2sSpotToChannelSendSendEchoScenario(
    private val config: RoleConfig,
    private val spots: ZLinkRouteClient,
    private val manager: ZLinkSpotManager,
    private val measurement: Measurement,
    private val mesh: ZLinkRouteMeshRuntime,
    private val readiness: ObjectsReadiness,
    private val correlations: SendSendCorrelation,
    private val metrics: ScenarioMetrics,
) {
    private lateinit var sequences: AtomicLongArray
    private lateinit var streamTargets: List<String>

    companion object {
        fun run(config: RoleConfig) {
            require(config.role() == "spot" && config.source())
            val app = KotlinSpotRole.application(config, S2sSendSendSpot::class.java, true)
            app.bean(ScenarioMetrics::class.java) {
                ScenarioMetrics(app.measurement())
                    .counters(
                        "driver.issued",
                        "driver.notStarted",
                        "driver.failed",
                        "spot.applicationHandlerEntries",
                    )
                    .latency("driverLatencyMs", "driver.latency")
                    .spotInternalsUnsupported()
            }
            app.bean(SendSendCorrelation::class.java)
            app.bean(S2sSpotToChannelSendSendEchoScenario::class.java)
                .workload(
                    S2sSpotToChannelSendSendEchoScenario::class.java,
                    S2sSpotToChannelSendSendEchoScenario::run,
                )
            app.start()
                .getBean(S2sSpotToChannelSendSendEchoScenario::class.java)
                .prepare()
                .exceptionally { error ->
                    app.measurement().recordDiagnostic(error)
                    null
                }
        }
    }

    fun prepare(): CompletionStage<Void> = completionStage {
        val created = KotlinSpotRole.createSpots(config, manager, mesh)
        Polling.until(
                {
                    mesh.snapshot(config.meshName()).channels().any {
                        it.channelName() == config.channelName() &&
                            it.isReady() &&
                            it.readyTargetCount() > 0
                    }
                },
                10,
                config.workload().setupTimeoutMs().toLong(),
            )
            .await()
        sequences = AtomicLongArray(config.workload().logicalStreams())
        streamTargets = planStreamTargets(config.spotIds(), config.workload().logicalStreams())
        val probes = ArrayList<Any>()
        config.spotIds().forEachIndexed { index, spotId ->
            val request =
                measurement
                    .request(index, sequences.incrementAndGet(index % sequences.length()), true)
                    .withReturnSpotId(spotId)
            val driven =
                spots
                    .kotlin()
                    .requestToSpot<PerfDriveReply>(spotId, PerfDriveRequest(request))
                    .timeout(measurement.callTimeout(true))
                    .await()
            check(driven.started()) { "Send/send setup probe was not started." }
            val entry =
                correlations.find(request.correlationId())
                    ?: throw PerfValidationException(
                        "UnknownCorrelation",
                        "Setup probe registered no correlation.",
                    )
            val result = correlations.completeAsync(entry).await()
            check(result.error() == null) {
                "Send/send setup probe failed: ${result.error()?.message}"
            }
            probes.add(mapOf("correlationId" to request.correlationId()))
        }
        measurement.setupEvidence(
            listOf(
                Evidence.of(
                    "typedProbeEcho",
                    "Kotlin Spot sendToChannel -> Channel sendToSpot -> Spot return handler",
                    probes,
                )
            )
        )
        readiness.set(true, "", listOf(created))
    }

    fun run(): CompletionStage<Void> = completionStage {
        runTerminalStreams(config.workload().logicalStreams(), measurement::canIssue) { stream ->
            val spotId = streamTargets[stream]
            val request =
                measurement
                    .request(stream, sequences.incrementAndGet(stream), false)
                    .withReturnSpotId(spotId)
            metrics.count("driver.issued")
            val driverStarted = PerfClock.now()
            var driverCompletedTicks = 0L
            var driverError: Throwable? = null
            val driven =
                try {
                    val reply =
                        spots
                            .kotlin()
                            .requestToSpot<PerfDriveReply>(spotId, PerfDriveRequest(request))
                            .timeout(measurement.callTimeout(true))
                            .await()
                    driverCompletedTicks = PerfClock.now()
                    reply
                } catch (error: Throwable) {
                    metrics.count("driver.failed")
                    measurement.recordDiagnostic(error)
                    driverError = error
                    null
                }
            if (driven != null && !driven.started()) {
                metrics.count("driver.notStarted")
                return@runTerminalStreams true
            }
            val fatal = driverError?.takeIf { it is CancellationException || it !is Exception }
            val entry = correlations.find(request.correlationId())
            if (entry == null) {
                if (driven?.started() == true) {
                    measurement.recordDiagnostic(
                        PerfValidationException(
                            "UnknownCorrelation",
                            "The started drive registered no correlation.",
                        )
                    )
                }
            } else {
                correlations
                    .completeAsync(entry)
                    .thenAccept { result ->
                        val counted =
                            measurement.completeOperation(
                                entry.startedTicks(),
                                result.error(),
                                result.completedTicks(),
                            )
                        if (driverError == null && counted) {
                            metrics.record("driverLatencyMs", driverStarted, driverCompletedTicks)
                        }
                    }
                    .whenComplete { _, error ->
                        if (error != null) measurement.recordDiagnostic(error)
                    }
            }
            if (fatal != null) throw fatal

            true
        }
    }
}
