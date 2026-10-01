package systems.zlink.framework.perf.servers.channel

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
import systems.zlink.framework.perf.Polling
import systems.zlink.framework.perf.RoleConfig
import systems.zlink.framework.perf.ScenarioMetrics
import systems.zlink.framework.perf.ServerApplication
import systems.zlink.framework.perf.SpotSetup
import systems.zlink.framework.perf.kotlin.completionStage
import systems.zlink.framework.perf.kotlin.planStreamTargets
import systems.zlink.framework.spots.ZLinkSpotManager

internal class KotlinS2sChannelToSpotRequestEchoScenario(
    private val config: RoleConfig,
    private val measurement: Measurement,
    private val route: ZLinkRouteClient,
    private val manager: ZLinkSpotManager,
    private val mesh: ZLinkRouteMeshRuntime,
    private val readiness: ObjectsReadiness,
) {
    private lateinit var sequences: AtomicLongArray
    private lateinit var streamTargets: List<String>

    companion object {
        fun run(config: RoleConfig) {
            require(config.role() == "channel" && config.source() && config.scenario() == "s2s-channel-to-spot-request-echo")
            val app = ServerApplication.create(config).configure { options ->
                options.enableKotlinPerfHandlers()
                ServerApplication.routeMesh(options, config, "perf-channel").objects().client()
            }.bean(ObjectsReadiness::class.java) {
                ObjectsReadiness(false, "No User Spot has been found through the public manager yet.")
            }.bean(KotlinS2sChannelToSpotRequestEchoScenario::class.java)
                .workload(KotlinS2sChannelToSpotRequestEchoScenario::class.java, KotlinS2sChannelToSpotRequestEchoScenario::run)
            app.bean(ScenarioMetrics::class.java) { ScenarioMetrics(app.measurement()).spotInternalsUnsupported() }
            app.start().getBean(KotlinS2sChannelToSpotRequestEchoScenario::class.java).prepare()
                .exceptionally { error -> app.measurement().recordDiagnostic(error); null }
        }
    }

    fun prepare(): CompletionStage<Void> = completionStage {
        Polling.until({ mesh.snapshot(config.meshName()).isReady() }, 10, config.workload().setupTimeoutMs().toLong()).await()
        val found = SpotSetup.findAll(manager, config).await()
        sequences = AtomicLongArray(config.workload().logicalStreams())
        streamTargets = planStreamTargets(config.spotIds(), config.workload().logicalStreams())
        val observations = ArrayList<Any>()
        config.spotIds().forEachIndexed { index, spotId ->
            val request = measurement.request(index, sequences.incrementAndGet(index % sequences.length()), true)
            val reply = route.kotlin().requestToSpot<PerfEchoReply>(spotId, request)
                .timeout(Duration.ofMillis(config.workload().requestTimeoutMs().toLong())).await()
            PayloadPattern.validateIdentity(request, reply)
            measurement.pattern().validate(reply.payload())
            observations.add(mapOf("correlationId" to request.correlationId(), "receivedTicks" to reply.receivedTicks(), "clockDomainId" to reply.clockDomainId()))
        }
        readiness.set(true, "", listOf(Evidence.of("spotFind", "ZLinkSpotManager.find", found)))
        measurement.setupEvidence(listOf(Evidence.of("typedProbeEcho", "Kotlin RouteClient.requestToSpot<PerfEchoReply>().await()", observations)))
    }

    fun run(): CompletionStage<Void> = completionStage {
        coroutineScope {
            repeat(config.workload().logicalStreams()) { stream ->
                repeat(config.workload().inflight()) {
                    launch(Dispatchers.IO) {
                        while (measurement.canIssue()) {
                            val spotId = streamTargets[stream]
                            val base = measurement.request(stream, sequences.incrementAndGet(stream), false)
                            val started = measurement.beginOperation("request")
                            if (started < 0) break
                            try {

                                    val request = base.withSentTicks(started)
                                    val reply = route.kotlin().requestToSpot<PerfEchoReply>(spotId, request)
                                        .timeout(Duration.ofMillis(config.workload().requestTimeoutMs().toLong())).await()
                                    PayloadPattern.validateIdentity(request, reply)
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
}
