package systems.zlink.framework.perf.servers.channel

import java.net.URI
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
import systems.zlink.framework.kotlin.requestToChannel
import systems.zlink.framework.monitoring.ZLinkClientServerRuntime
import systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime
import systems.zlink.framework.perf.Evidence
import systems.zlink.framework.perf.Measurement
import systems.zlink.framework.perf.PayloadPattern
import systems.zlink.framework.perf.PerfEchoReply
import systems.zlink.framework.perf.RoleConfig
import systems.zlink.framework.perf.ServerApplication
import systems.zlink.framework.perf.Polling
import systems.zlink.framework.perf.kotlin.completionStage

internal class KotlinChannelEchoOnlyScenario(
    private val config: RoleConfig,
    private val measurement: Measurement,
    private val client: ZLinkRouteClient,
    private val meshRuntime: ZLinkRouteMeshRuntime,
    private val channelRuntime: ZLinkClientServerRuntime,
) {
    private lateinit var sequences: AtomicLongArray

    companion object {
        fun run(config: RoleConfig) {
            require(config.role() == "channel")
            val app = ServerApplication.create(config).configure { options ->
                options.enableKotlinPerfHandlers()
                if (config.topology() == "routemesh") {
                    val mesh = ServerApplication.routeMesh(options, config, "perf-channel")
                    if (config.source()) {
                        mesh.channelName(config.channelName()).client()
                        mesh.peerConnections().connect(config.peerEndpoint())
                    } else {
                        mesh.channelName(config.channelName()).server().addHandlerGroup(BASELINE_GROUP)
                    }
                } else {
                    val channel = options.addClientServerChannel(config.channelName())
                    if (config.source()) channel.client().connect(config.peerEndpoint())
                    else channel.server().listen(URI.create(config.listenerEndpoint()).port).addHandlerGroup(BASELINE_GROUP)
                }
            }.bean(KotlinChannelEchoHandler::class.java)
            if (config.source()) {
                app.bean(KotlinChannelEchoOnlyScenario::class.java)
                    .workload(KotlinChannelEchoOnlyScenario::class.java, KotlinChannelEchoOnlyScenario::run)
            }
            val context = app.start()
            if (config.source()) context.getBean(KotlinChannelEchoOnlyScenario::class.java).prepare()
                .exceptionally { error -> app.measurement().recordDiagnostic(error); null }
        }
    }

    private fun ready(): Boolean = if (config.topology() == "routemesh") {
        val status = meshRuntime.snapshot(config.meshName())
        status.isReady() && status.channels().any { channel ->
            channel.channelName() == config.channelName() && channel.isReady() && channel.readyTargetCount() > 0
        }
    } else {
        val status = channelRuntime.snapshot(config.channelName())
        status.isReady() && status.readyTargetCount() > 0
    }

    fun prepare(): CompletionStage<Void> = completionStage {
        Polling.until({ ready() }, 5, config.workload().setupTimeoutMs().toLong()).await()
        sequences = AtomicLongArray(config.workload().logicalStreams())
        val request = measurement.request(0, sequences.incrementAndGet(0), true)
        val reply = client.kotlin().requestToChannel<PerfEchoReply>(config.channelName(), request)
            .timeout(Duration.ofMillis(config.workload().requestTimeoutMs().toLong())).await()
        PayloadPattern.validateIdentity(request, reply)
        measurement.pattern().validate(reply.payload())
        measurement.setupEvidence(listOf(Evidence.of("typedProbeEcho", "Kotlin route requestToChannel<PerfEchoReply>().await",
            mapOf("correlationId" to request.correlationId(), "receivedTicks" to reply.receivedTicks(), "clockDomainId" to reply.clockDomainId()))))
    }

    fun run(): CompletionStage<Void> = completionStage {
        coroutineScope {
            repeat(config.workload().logicalStreams()) { stream ->
                repeat(config.workload().inflight()) {
                    launch(Dispatchers.IO) {
                        while (measurement.canIssue()) {
                            val request = measurement.request(stream, sequences.incrementAndGet(stream), false)
                            val started = measurement.beginOperation()
                            if (started < 0) break
                            try {

                                    val sent = request.withSentTicks(started)
                                    val reply = client.kotlin().requestToChannel<PerfEchoReply>(config.channelName(), sent)
                                        .timeout(Duration.ofMillis(config.workload().requestTimeoutMs().toLong())).await()
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
}
