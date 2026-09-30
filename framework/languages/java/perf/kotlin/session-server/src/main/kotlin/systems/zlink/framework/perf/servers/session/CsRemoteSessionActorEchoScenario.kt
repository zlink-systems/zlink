package systems.zlink.framework.perf.servers.session

import java.time.Duration
import kotlinx.coroutines.future.await
import systems.zlink.framework.actors.ZLinkActorCreateResult
import systems.zlink.framework.actors.ZLinkActorManager
import systems.zlink.framework.kotlin.ZLinkSuspendingSession
import systems.zlink.framework.kotlin.decode
import systems.zlink.framework.kotlin.kotlin
import systems.zlink.framework.kotlin.bindOrGetActor
import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.framework.perf.Evidence
import systems.zlink.framework.perf.Measurement
import systems.zlink.framework.perf.ObjectsReadiness
import systems.zlink.framework.perf.PerfClock
import systems.zlink.framework.perf.PerfEchoRequest
import systems.zlink.framework.perf.PerfActorType
import systems.zlink.framework.perf.RoleConfig
import systems.zlink.framework.perf.ServerApplication
import systems.zlink.framework.streams.ZLinkSessionActor
import systems.zlink.framework.streams.ZLinkSessionContext
import systems.zlink.framework.streams.ZLinkSessionDispatchContext
import systems.zlink.framework.streams.ZLinkSessionPacketDispatcher
import systems.zlink.framework.streams.ZLinkStreamError

internal object CsRemoteSessionActorEchoScenario {
    fun run(config: RoleConfig) {
        require(config.role() == "session" && !config.source() && config.scenario() == "cs-remote-session-actor-echo")
        val app = ServerApplication.create(config)
            .bean(ObjectsReadiness::class.java) { ObjectsReadiness(false, "No Actor is bound to a session yet.") }
            .bean(KotlinSessionActorSetup::class.java)
            .configure { options ->
                ServerApplication.routeMesh(options, config, "perf-session").objects().client()
                options.addStreamNode("perf-session")
                    .bind(config.transportEndpoints().get("stream"))
                    .enableActorDispatch()
                    .registerSession(KotlinPerfSessionActorRelayHandler::class.java)
            }
        app.start()
    }
}

class KotlinPerfSessionActorRelayHandler(
    private val session: ZLinkSessionContext,
    @Suppress("unused") private val handlers: ZLinkSessionPacketDispatcher<ZLinkSessionContext>,
    private val measurement: Measurement,
    private val setup: KotlinSessionActorSetup,
) : ZLinkSuspendingSession() {
    private var binding: ZLinkSessionActor? = null

    override fun context(): ZLinkSessionContext = session

    override suspend fun onErrorSuspending(error: ZLinkStreamError) {
        measurement.recordDiagnostic(IllegalStateException("STREAM ${error.error()}: ${error.message()}"))
    }

    override suspend fun onDispatchSuspending(dispatch: ZLinkSessionDispatchContext, payload: ZLinkMessage) {
        val actor = dispatch.actor() ?: binding ?: setup.bind(session, payload)
        binding = actor
        actor.kotlin().relay(dispatch, payload).await()
    }
}

class KotlinSessionActorSetup(
    private val config: RoleConfig,
    private val measurement: Measurement,
    private val readiness: ObjectsReadiness,
    private val actors: ZLinkActorManager,
) {
    private val gate = Any()
    private var created = 0L
    private var existing = 0L
    private var bound = 0L
    private var failed = 0L
    private var createNs = 0L
    private var createMaxNs = 0L
    private var bindNs = 0L
    private var bindMaxNs = 0L

    suspend fun bind(session: ZLinkSessionContext, payload: ZLinkMessage): ZLinkSessionActor {
        check(measurement.phase() == "setup") { "Session Actor binding is only prepared by its setup probe." }
        try {
            val request = payload.decode<PerfEchoRequest>()
            require(request.clientId() in config.actorIds().indices) { "clientId has no Actor ID in this cell." }
            val marks = LongArray(3)
            marks[0] = PerfClock.now()
            val result = actors.kotlin().getOrCreate(config.actorIds()[request.clientId()], PerfActorType.NAME)
                .inMesh(config.meshName())
                .timeout(Duration.ofMillis(config.workload().setupTimeoutMs().toLong()))
                .await()
            val (actorRef, wasCreated) = when (result) {
                is ZLinkActorCreateResult.Created -> result.actor() to true
                is ZLinkActorCreateResult.Existing -> result.actor() to false
                else -> throw IllegalStateException("Actor creation was rejected.")
            }
            marks[1] = PerfClock.now()
            val actor: ZLinkSessionActor = session.actors().bindOrGetActor(actorRef)
            marks[2] = PerfClock.now()
            record(wasCreated, marks[1] - marks[0], marks[2] - marks[1])
            return actor
        } catch (error: Exception) {
            measurement.recordDiagnostic(error)
            synchronized(gate) { failed++ }
            publish()
            throw error
        }
    }

    private fun record(wasCreated: Boolean, createLatency: Long, bindLatency: Long) {
        synchronized(gate) {
            if (wasCreated) created++ else existing++
            bound++
            createNs += createLatency
            createMaxNs = maxOf(createMaxNs, createLatency)
            bindNs += bindLatency
            bindMaxNs = maxOf(bindMaxNs, bindLatency)
        }
        publish()
    }

    private fun publish() {
        synchronized(gate) {
            val observed = linkedMapOf<String, Any>(
                "created" to created,
                "existing" to existing,
                "bound" to bound,
                "failed" to failed,
                "expectedActors" to config.actorIds().size,
                "createMeanMs" to if (bound == 0L) 0 else createNs / 1e6 / bound,
                "createMaxMs" to createMaxNs / 1e6,
                "bindMeanMs" to if (bound == 0L) 0 else bindNs / 1e6 / bound,
                "bindMaxMs" to bindMaxNs / 1e6,
            )
            readiness.set(
                bound > 0 && failed == 0L,
                if (failed > 0) "Actor create or bind failed." else "No Actor is bound to a session yet.",
                listOf(Evidence.of("actorCreateAndBind", "Kotlin getOrCreate(...).await + bindOrGetActor(ref)", observed)),
            )
            if (bound > 0) {
                measurement.setupEvidence(listOf(Evidence.of("relayAdmission", "ZLinkSessionActor.relay", mapOf("bound" to bound))))
            }
        }
    }
}
