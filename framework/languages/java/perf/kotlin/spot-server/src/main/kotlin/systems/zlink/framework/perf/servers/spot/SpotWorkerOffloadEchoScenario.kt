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
import systems.zlink.framework.perf.PayloadPattern
import systems.zlink.framework.perf.PerfEchoReply
import systems.zlink.framework.perf.RoleConfig
import systems.zlink.framework.perf.ScenarioMetrics
import systems.zlink.framework.perf.kotlin.completionStage
import systems.zlink.framework.perf.kotlin.planStreamTargets
import systems.zlink.framework.spots.ZLinkSpotManager

class SpotWorkerOffloadEchoScenario(
    private val config: RoleConfig,
    private val spots: ZLinkRouteClient,
    private val manager: ZLinkSpotManager,
    private val measurement: Measurement,
    private val mesh: ZLinkRouteMeshRuntime,
    private val readiness: ObjectsReadiness,
) {
    private lateinit var sequences: AtomicLongArray
    private lateinit var streamTargets: List<String>

    companion object {
        fun run(config: RoleConfig) {
            require(config.role() == "spot" && config.source() && config.worker() != null)
            val worker = config.worker()
            val app =
                KotlinSpotRole.application(config, SpotWorkerOffloadSpot::class.java, false) {
                    options ->
                    options
                        .configureWorkers()
                        .minThreads(worker.minThreads())
                        .maxThreads(worker.maxThreads())
                        .idleTimeout(Duration.ofMillis(worker.idleTimeoutMs().toLong()))
                }
            val applied =
                mapOf(
                    "minThreads" to worker.minThreads(),
                    "maxThreads" to worker.maxThreads(),
                    "idleTimeoutMs" to worker.idleTimeoutMs(),
                )
            val workerOptions =
                linkedMapOf<String, Any?>(
                    "algorithm" to worker.algorithm(),
                    "taskMillis" to worker.taskMillis(),
                    "applied" to applied,
                    "callDeadlineRule" to "phaseEnd+drainTimeoutMs",
                )
            app.bean(ScenarioMetrics::class.java) {
                ScenarioMetrics(app.measurement())
                    .counters("spot.applicationHandlerEntries", "spot.applicationYieldCalls")
                    .latency("workerCallLatencyMs", "worker.callLatency")
                    .latency("workerSubmitToStartMs", "worker.submitToStart")
                    .latency("workerTaskLatencyMs", "worker.taskLatency")
                    .latency("workerResultToContinuationMs", "worker.resultToContinuation")
                    .unsupported(
                        "PUBLIC_OBSERVATION_UNSUPPORTED",
                        "Public worker options are settings; no queue depth snapshot exists.",
                        "worker.pool.queueDepth.max",
                        "worker.pool.queueDepth.mean",
                    )
                    .spotInternalsUnsupported()
                    .provenance("workerOptions", workerOptions)
            }
            app.bean(SpotWorkerOffloadEchoScenario::class.java)
                .workload(
                    SpotWorkerOffloadEchoScenario::class.java,
                    SpotWorkerOffloadEchoScenario::run,
                )
            app.start()
                .getBean(SpotWorkerOffloadEchoScenario::class.java)
                .prepare()
                .exceptionally { error ->
                    app.measurement().recordDiagnostic(error)
                    null
                }
        }
    }

    fun prepare(): CompletionStage<Void> = completionStage {
        val created = KotlinSpotRole.createSpots(config, manager, mesh)
        sequences = AtomicLongArray(config.workload().logicalStreams())
        streamTargets = planStreamTargets(config.spotIds(), config.workload().logicalStreams())
        val probes = ArrayList<Any>()
        config.spotIds().forEachIndexed { index, spotId ->
            val request =
                measurement.request(
                    index,
                    sequences.incrementAndGet(index % sequences.length()),
                    true,
                )
            val reply =
                spots
                    .kotlin()
                    .requestToSpot<PerfEchoReply>(spotId, request)
                    .timeout(measurement.callTimeout())
                    .await()
            PayloadPattern.validateIdentity(request, reply)
            measurement.pattern().validate(reply.payload())
            probes.add(
                mapOf(
                    "correlationId" to request.correlationId(),
                    "receivedTicks" to reply.receivedTicks(),
                    "clockDomainId" to reply.clockDomainId(),
                )
            )
        }
        val typedProbe =
            Evidence.of(
                "typedProbeEcho",
                "Kotlin requestToSpot -> runCpuWorker Kotlin await/yield",
                probes,
            )
        measurement.setupEvidence(listOf(typedProbe))
        readiness.set(true, "", listOf(created))
    }

    fun run(): CompletionStage<Void> = completionStage {
        coroutineScope {
            repeat(config.workload().logicalStreams()) { stream ->
                launch(Dispatchers.IO) {
                    while (measurement.canIssue()) {
                        val request =
                            measurement.request(stream, sequences.incrementAndGet(stream), false)
                        val started = measurement.beginOperation()
                        if (started < 0) break
                        try {

                            val sent = request.withSentTicks(started)
                            val reply =
                                spots
                                    .kotlin()
                                    .requestToSpot<PerfEchoReply>(streamTargets[stream], sent)
                                    .timeout(measurement.callTimeout())
                                    .await()
                            PayloadPattern.validateIdentity(sent, reply)
                            measurement.pattern().validate(reply.payload())
                            measurement.completeOperation(started)
                        } catch (error: Exception) {
                            measurement.completeOperation(started, error)
                            if (error is CancellationException) throw error
                        }
                    }
                }
            }
        }
    }
}
