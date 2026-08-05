package systems.zlink.e2e.kotlin.spotservice.session.handlers

import systems.zlink.e2e.kotlin.spotservice.Contracts
import systems.zlink.e2e.kotlin.spotservice.ScenarioState
import systems.zlink.framework.streams.ZLinkSessionContext
import systems.zlink.framework.streams.ZLinkSessionDispatchContext
import kotlinx.coroutines.delay
import systems.zlink.framework.kotlin.ZLinkSuspendingTypedSessionPacketHandler

class SlowSessionHandler(
    private val evidence: ScenarioState
) : ZLinkSuspendingTypedSessionPacketHandler<ZLinkSessionContext, Contracts.SlowSessionReq> {
    override fun packetName(): String = "SlowSessionReq"

    override fun messageType(): Class<Contracts.SlowSessionReq> = Contracts.SlowSessionReq::class.java

    override suspend fun handle(
        context: ZLinkSessionContext,
        dispatch: ZLinkSessionDispatchContext,
        request: Contracts.SlowSessionReq
    ) {
        delay(request.delayMilliseconds.coerceAtLeast(0).toLong())
        evidence.record("SlowSessionHandled", "session", request.value)
        try {
            context.client()
                .reply(Contracts.SlowSessionRes(request.value))
                .submit()
        } catch (error: Exception) {
            evidence.record("SlowSessionReplyFailed", "session", "${request.value}/${error.javaClass.simpleName}")
        }
    }
}
