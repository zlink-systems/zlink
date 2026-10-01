package systems.zlink.framework.perf.servers.actorcaller

import java.time.Duration
import java.util.concurrent.CompletionStage
import java.util.concurrent.atomic.AtomicLongArray
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.future.await
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Semaphore
import kotlinx.coroutines.sync.withPermit
import systems.zlink.framework.actors.ZLinkActorClient
import systems.zlink.framework.perf.Evidence
import systems.zlink.framework.perf.Measurement
import systems.zlink.framework.perf.ObjectsReadiness
import systems.zlink.framework.perf.PayloadPattern
import systems.zlink.framework.perf.PerfEchoReply
import systems.zlink.framework.perf.RoleConfig
import systems.zlink.framework.perf.ServerApplication
import systems.zlink.framework.perf.kotlin.completionStage
import systems.zlink.framework.kotlin.kotlin
import systems.zlink.framework.kotlin.requestToActor

class ActorNoBindRequestEchoScenario(
    private val config: RoleConfig,
    private val actorClient: ZLinkActorClient,
    private val measurement: Measurement,
    private val setup: ActorCallerSetup,
    private val readiness: ObjectsReadiness,
) {
    private lateinit var sequences: AtomicLongArray

    companion object {
        fun run(config: RoleConfig) {
            val app = ServerApplication.create(config)
                .configure { options -> ServerApplication.routeMesh(options, config, "perf-actor-caller").objects().client() }
                .bean(ObjectsReadiness::class.java) { ObjectsReadiness(false, "Actors are not yet created and probed through the public API.") }
                .bean(ActorCallerSetup::class.java)
                .bean(ActorNoBindRequestEchoScenario::class.java)
                .workload(ActorNoBindRequestEchoScenario::class.java, ActorNoBindRequestEchoScenario::run)
            val context = app.start()
            context.getBean(ActorNoBindRequestEchoScenario::class.java).prepare()
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
                        val request = measurement.request(stream, sequences.incrementAndGet(stream), true)
                        val reply = actorClient.kotlin().requestToActor<PerfEchoReply>(actorId, request)
                            .timeout(Duration.ofMillis(config.workload().setupTimeoutMs().toLong())).await()
                        PayloadPattern.validateIdentity(request, reply)
                        measurement.pattern().validate(reply.payload())
                    }
                }
            }
        }
        val typedProbe = Evidence.of("typedProbeEcho", "Kotlin Actor wrapper requestToActor<PerfEchoReply>().await()",
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
                            val started = measurement.beginOperation()
                            if (started < 0) break
                            try {

                                    val sent = request.withSentTicks(started)
                                    val reply = actorClient.kotlin().requestToActor<PerfEchoReply>(config.actorIds()[stream], sent)
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
