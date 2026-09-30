package systems.zlink.framework.perf.servers.session

import kotlinx.coroutines.future.await
import systems.zlink.framework.kotlin.ZLinkSuspendingSession
import systems.zlink.framework.kotlin.decode
import systems.zlink.framework.kotlin.kotlin
import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.framework.perf.Evidence
import systems.zlink.framework.perf.Measurement
import systems.zlink.framework.perf.PayloadPattern
import systems.zlink.framework.perf.PerfClock
import systems.zlink.framework.perf.PerfEchoRequest
import systems.zlink.framework.perf.RoleConfig
import systems.zlink.framework.perf.ServerApplication
import systems.zlink.framework.streams.ZLinkSessionContext
import systems.zlink.framework.streams.ZLinkSessionDispatchContext
import systems.zlink.framework.streams.ZLinkSessionPacketDispatcher
import systems.zlink.framework.streams.ZLinkStreamError

internal object SessionEchoOnlyScenario {
    fun run(config: RoleConfig) {
        require(config.role() == "session" && !config.source() && config.scenario() == "session-echo-only")
        val app = ServerApplication.create(config).configure { options ->
            options.addStreamNode("perf-session")
                .bind(config.transportEndpoints().get("stream"))
                .registerSession(KotlinPerfSessionEchoHandler::class.java)
        }
        app.start()
    }
}

class KotlinPerfSessionEchoHandler(
    private val session: ZLinkSessionContext,
    @Suppress("unused") private val handlers: ZLinkSessionPacketDispatcher<ZLinkSessionContext>,
    private val measurement: Measurement,
) : ZLinkSuspendingSession() {
    override fun context(): ZLinkSessionContext = session

    override suspend fun onErrorSuspending(error: ZLinkStreamError) {
        measurement.recordDiagnostic(IllegalStateException("STREAM ${error.error()}: ${error.message()}"))
    }

    override suspend fun onDispatchSuspending(dispatch: ZLinkSessionDispatchContext, payload: ZLinkMessage) {
        val request = payload.decode<PerfEchoRequest>()
        val received = PerfClock.now()
        measurement.handlerEnter()
        try {
            measurement.validateRequest(request)
            val reply = PayloadPattern.reply(request, received)
            measurement.recordReply(request)
            session.client().kotlin().reply(reply).await()
            if (measurement.phase() == "setup") {
                measurement.setupEvidence(listOf(Evidence.of("typedProbeReply", "Kotlin session reply(reply).await", request.correlationId())))
            }
        } catch (error: RuntimeException) {
            measurement.recordDiagnostic(error)
            throw error
        } finally {
            measurement.handlerExit()
        }
    }
}
