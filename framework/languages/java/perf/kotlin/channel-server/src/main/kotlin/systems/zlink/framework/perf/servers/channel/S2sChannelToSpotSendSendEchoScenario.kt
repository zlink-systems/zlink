package systems.zlink.framework.perf.servers.channel

import java.util.concurrent.CompletionStage
import java.util.concurrent.atomic.AtomicLongArray
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.future.await
import kotlinx.coroutines.launch
import org.springframework.beans.factory.ObjectProvider
import systems.zlink.framework.channels.ZLinkRouteClient
import systems.zlink.framework.kotlin.kotlin
import systems.zlink.framework.kotlin.sendToSpot
import systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime
import systems.zlink.framework.perf.Evidence
import systems.zlink.framework.perf.Measurement
import systems.zlink.framework.perf.ObjectsReadiness
import systems.zlink.framework.perf.PerfClock
import systems.zlink.framework.perf.Polling
import systems.zlink.framework.perf.RoleConfig
import systems.zlink.framework.perf.ScenarioMetrics
import systems.zlink.framework.perf.SendSendCorrelation
import systems.zlink.framework.perf.ServerApplication
import systems.zlink.framework.perf.SpotSetup
import systems.zlink.framework.perf.kotlin.completionStage
import systems.zlink.framework.spots.ZLinkSpotManager

internal class KotlinS2sChannelToSpotSendSendEchoScenario(
    private val config: RoleConfig,
    private val measurement: Measurement,
    private val route: ZLinkRouteClient,
    private val manager: ZLinkSpotManager,
    private val mesh: ZLinkRouteMeshRuntime,
    private val readiness: ObjectsReadiness,
    private val correlations: ObjectProvider<SendSendCorrelation>,
) {
    private lateinit var sequences: AtomicLongArray

    companion object {
        fun run(config: RoleConfig) {
            require(config.role() == "channel" && config.source() && config.scenario() == "s2s-channel-to-spot-send-send-echo")
            val app = ServerApplication.create(config).configure { options ->
                options.enableKotlinPerfHandlers()
                val node = ServerApplication.routeMesh(options, config, "perf-channel")
                node.objects().client()
                node.channelName(config.channelName()).server().addHandlerGroup(RETURN_GROUP)
            }.bean(ObjectsReadiness::class.java) {
                ObjectsReadiness(false, "No User Spot has been found through the public manager yet.")
            }.bean(KotlinS2sChannelToSpotSendSendEchoScenario::class.java)
                .workload(KotlinS2sChannelToSpotSendSendEchoScenario::class.java, KotlinS2sChannelToSpotSendSendEchoScenario::run)
            app.bean(ScenarioMetrics::class.java) { ScenarioMetrics(app.measurement()).spotInternalsUnsupported() }
            app.bean(ChannelReturnHandler::class.java)
            app.bean(SendSendCorrelation::class.java)
            app.start().getBean(KotlinS2sChannelToSpotSendSendEchoScenario::class.java).prepare()
                .exceptionally { error -> app.measurement().recordDiagnostic(error); null }
        }
    }

    fun prepare(): CompletionStage<Void> = completionStage {
        Polling.until({ mesh.snapshot(config.meshName()).isReady() }, 10, config.workload().setupTimeoutMs().toLong()).await()
        val found = SpotSetup.findAll(manager, config).await()
        sequences = AtomicLongArray(config.workload().logicalStreams())
        val observations = ArrayList<Any>()
        config.spotIds().forEachIndexed { index, spotId ->
            val request = measurement.request(index, sequences.incrementAndGet(index % sequences.length()), true)
                .withReturnChannel(config.channelName())
            val correlation = correlations.getObject().register(request, PerfClock.now())
            route.kotlin().sendToSpot(spotId, request).await()
            correlations.getObject().firstSendEnded(correlation, null)
            val result = correlations.getObject().completeAsync(correlation).await()
            check(result.error() == null) { "Send/send setup probe failed: ${result.error()?.message}" }
            observations.add(mapOf("correlationId" to request.correlationId()))
        }
        readiness.set(true, "", listOf(Evidence.of("spotFind", "ZLinkSpotManager.find", found)))
        measurement.setupEvidence(listOf(Evidence.of("typedProbeEcho", "Kotlin RouteClient.sendToSpot(...).await()", observations)))
    }

    fun run(): CompletionStage<Void> = completionStage {
        coroutineScope {
            repeat(config.workload().logicalStreams()) { stream ->
                repeat(config.workload().inflight()) {
                    launch(Dispatchers.IO) {
                        while (measurement.canIssue()) {
                            val spotId = config.spotIds()[stream % config.spotIds().size]
                            val base = measurement.request(stream, sequences.incrementAndGet(stream), false)
                            val started = measurement.beginOperation("send")
                            if (started < 0) break
                            val sent = base.withSentTicks(started).withReturnChannel(config.channelName())
                            val correlation = correlations.getObject().register(sent, started)
                            try {
                                route.kotlin().sendToSpot(spotId, sent).await()
                                correlations.getObject().firstSendEnded(correlation, null)
                            } catch (error: Exception) {
                                if (error is CancellationException) throw error
                                correlations.getObject().firstSendEnded(correlation, error)
                            }
                            val result = correlations.getObject().completeAsync(correlation).await()
                            measurement.completeOperation(started, result.error(), result.completedTicks())
                        }
                    }
                }
            }
        }
    }
}
