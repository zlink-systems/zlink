package systems.zlink.framework.perf.servers.actorcaller

import java.util.concurrent.CompletionStage
import java.util.concurrent.atomic.AtomicLongArray
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.future.await
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Semaphore
import kotlinx.coroutines.sync.withPermit
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.actors.ZLinkActorClient
import systems.zlink.framework.handlers.ZLinkHandlerGroup
import systems.zlink.framework.kotlin.ZLinkSuspendingSendHandler
import systems.zlink.framework.kotlin.addHandlersFromPackageOf
import systems.zlink.framework.kotlin.kotlin
import systems.zlink.framework.kotlin.useCoroutineHandlers
import systems.zlink.framework.perf.Evidence
import systems.zlink.framework.perf.Measurement
import systems.zlink.framework.perf.ObjectsReadiness
import systems.zlink.framework.perf.PerfClock
import systems.zlink.framework.perf.PerfEchoReply
import systems.zlink.framework.perf.RoleConfig
import systems.zlink.framework.perf.ScenarioMetrics
import systems.zlink.framework.perf.SendSendCorrelation
import systems.zlink.framework.perf.ServerApplication
import systems.zlink.framework.perf.kotlin.completionStage

private const val RETURN_GROUP = "perf-kotlin-actor-caller-return"

private class ActorCallerHandlerMarker

@ZLinkHandlerGroup(RETURN_GROUP)
class KotlinActorReturnHandler(private val correlations: SendSendCorrelation) : ZLinkSuspendingSendHandler<PerfEchoReply> {
    override suspend fun handle(message: PerfEchoReply, context: ZLinkMessageContext) {
        correlations.reply(message)
    }
}

class ActorNoBindSendSendEchoScenario(
    private val config: RoleConfig,
    private val actorClient: ZLinkActorClient,
    private val measurement: Measurement,
    private val setup: ActorCallerSetup,
    private val readiness: ObjectsReadiness,
    private val correlations: SendSendCorrelation,
    private val metrics: ScenarioMetrics,
) {
    private lateinit var sequences: AtomicLongArray

    companion object {
        fun run(config: RoleConfig) {
            val app = ServerApplication.create(config)
            app.configure { options ->
                options.addHandlersFromPackageOf<ActorCallerHandlerMarker>()
                options.useCoroutineHandlers(Dispatchers.IO)
                val mesh = ServerApplication.routeMesh(options, config, "perf-actor-caller")
                mesh.objects().client()
                mesh.channelName(config.channelName()).server().addHandlerGroup(RETURN_GROUP)
            }
            app.bean(ObjectsReadiness::class.java) { ObjectsReadiness(false, "Actors are not yet created and probed through the public API.") }
                .bean(ScenarioMetrics::class.java) { ScenarioMetrics(app.measurement()).latency("sourceAdmissionMs", "actor.sourceAdmission.latency") }
                .bean(SendSendCorrelation::class.java)
                .bean(ActorCallerSetup::class.java)
                .bean(ActorNoBindSendSendEchoScenario::class.java)
                .workload(ActorNoBindSendSendEchoScenario::class.java, ActorNoBindSendSendEchoScenario::run)
            val context = app.start()
            context.getBean(ActorNoBindSendSendEchoScenario::class.java).prepare()
                .exceptionally { error -> app.measurement().recordDiagnostic(error); null }
        }
    }

    fun prepare(): CompletionStage<Void> = completionStage {
        val created = setup.createActors()
        sequences = AtomicLongArray(config.workload().logicalStreams())
        val probes = Semaphore(config.workload().connectConcurrency())
        coroutineScope {
            config.actorIds().forEachIndexed { stream, actorId ->
                launch(Dispatchers.IO) {
                    probes.withPermit {
                        val request = measurement.request(stream, sequences.incrementAndGet(stream), true).withReturnChannel(config.channelName())
                        val entry = correlations.register(request, PerfClock.now())
                        actorClient.kotlin().sendToActor(actorId, request).await()
                        val result = correlations.completeAsync(entry).await()
                        check(result.error() == null) { "Actor send/send setup probe failed: ${result.error()?.message}" }
                    }
                }
            }
        }
        val typedProbe = Evidence.of("typedProbeEcho", "Kotlin Actor sendToActor(...).await() -> return Channel handler",
            mapOf("probes" to sequences.length(), "streams" to sequences.length()))
        measurement.setupEvidence(listOf(typedProbe))
        readiness.set(true, "", listOf(created, typedProbe))
    }

    fun run(): CompletionStage<Void> = completionStage {
        coroutineScope {
            repeat(config.workload().logicalStreams()) { stream ->
                repeat(config.workload().inflight()) {
                    launch(Dispatchers.IO) {
                        while (measurement.canIssue()) {
                            val request = measurement.request(stream, sequences.incrementAndGet(stream), false)
                                .withReturnChannel(config.channelName())
                            val started = measurement.beginOperation("send")
                            if (started < 0) break
                            val sent = request.withSentTicks(started)
                            val entry = correlations.register(sent, started)
                            try {
                                actorClient.kotlin().sendToActor(config.actorIds()[stream], sent).await()
                                metrics.record("sourceAdmissionMs", started, PerfClock.now())
                                correlations.firstSendEnded(entry, null)
                            } catch (error: Exception) {
                                if (error is CancellationException) throw error
                                correlations.firstSendEnded(entry, error)
                            }
                            val result = correlations.completeAsync(entry).await()
                            measurement.completeOperation(started, result.error(), result.completedTicks())
                        }
                    }
                }
            }
        }
    }
}
