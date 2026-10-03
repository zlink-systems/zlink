package systems.zlink.framework.perf.servers.spot

import java.time.Duration
import java.util.concurrent.CompletionStage
import java.util.concurrent.atomic.AtomicLongArray
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.future.await
import kotlinx.coroutines.launch
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
import systems.zlink.framework.perf.kotlin.completionStage
import systems.zlink.framework.perf.kotlin.planStreamTargets
import systems.zlink.framework.spots.ZLinkSpotManager

class S2sSpotToChannelRequestEchoScenario(
    private val config: RoleConfig,
    private val spots: ZLinkRouteClient,
    private val manager: ZLinkSpotManager,
    private val measurement: Measurement,
    private val mesh: ZLinkRouteMeshRuntime,
    private val readiness: ObjectsReadiness,
    private val metrics: ScenarioMetrics,
) {
    private lateinit var sequences: AtomicLongArray
    private lateinit var streamTargets: List<String>

    companion object {
        fun run(config: RoleConfig) {
            require(config.role() == "spot" && config.source())
            val app = KotlinSpotRole.application(config, S2sRemoteRequestSpot::class.java, true)
            app.bean(ScenarioMetrics::class.java) {
                ScenarioMetrics(app.measurement())
                    .counters("driver.issued", "driver.notStarted", "driver.failed", "spot.applicationHandlerEntries", "spot.applicationYieldCalls")
                    .latency("driverLatencyMs", "driver.latency")
                    .aliasLatency("latency", "spot.remoteCallLatency")
                    .spotInternalsUnsupported()
            }
            app.bean(S2sSpotToChannelRequestEchoScenario::class.java)
                .workload(S2sSpotToChannelRequestEchoScenario::class.java, S2sSpotToChannelRequestEchoScenario::run)
            app.start().getBean(S2sSpotToChannelRequestEchoScenario::class.java).prepare()
                .exceptionally { error -> app.measurement().recordDiagnostic(error); null }
        }
    }

    fun prepare(): CompletionStage<Void> = completionStage {
        val created = KotlinSpotRole.createSpots(config, manager, mesh)
        Polling.until({
            mesh.snapshot(config.meshName()).channels().any { it.channelName() == config.channelName() && it.isReady() && it.readyTargetCount() > 0 }
        }, 10, config.workload().setupTimeoutMs().toLong()).await()
        sequences = AtomicLongArray(config.workload().logicalStreams())
        streamTargets = planStreamTargets(config.spotIds(), config.workload().logicalStreams())
        val probes = ArrayList<Any>()
        config.spotIds().forEachIndexed { index, spotId ->
            val request = measurement.request(index, sequences.incrementAndGet(index % sequences.length()), true)
            val driven = spots.kotlin().requestToSpot<PerfDriveReply>(spotId, PerfDriveRequest(request))
                .timeout(Duration.ofMillis(config.workload().driverTimeoutMs().toLong())).await()
            val reply = driven.echo() ?: throw PerfValidationException("IdentityMismatch", "Setup probe did not reach Channel.")
            check(driven.started()) { "Setup probe was not started." }
            probes.add(mapOf("correlationId" to request.correlationId(), "receivedTicks" to reply.receivedTicks(), "clockDomainId" to reply.clockDomainId()))
        }
        measurement.setupEvidence(listOf(Evidence.of("typedProbeEcho", "Kotlin RouteClient requestToSpot -> Spot requestToChannel", probes)))
        readiness.set(true, "", listOf(created))
    }

    fun run(): CompletionStage<Void> = completionStage {
        coroutineScope {
            repeat(config.workload().logicalStreams()) { stream ->
                repeat(config.workload().inflight()) {
                    launch(Dispatchers.IO) {
                        while (measurement.canIssue()) {
                            val echo = measurement.request(stream, sequences.incrementAndGet(stream), false)
                            metrics.count("driver.issued")
                            val driverStarted = PerfClock.now()
                            val driven = try {
                                spots.kotlin().requestToSpot<PerfDriveReply>(
                                    streamTargets[stream], PerfDriveRequest(echo),
                                ).timeout(Duration.ofMillis(config.workload().driverTimeoutMs().toLong())).await()
                            } catch (error: Throwable) {
                                metrics.count("driver.failed")
                                measurement.recordDiagnostic(error)
                                if (error is CancellationException || error !is Exception) throw error
                                continue
                            }
                            if (!driven.started()) metrics.count("driver.notStarted")
                            if (driven.echo() != null) {
                                metrics.record("driverLatencyMs", driverStarted, PerfClock.now())
                            }
                        }
                    }
                }
            }
        }
    }
}
