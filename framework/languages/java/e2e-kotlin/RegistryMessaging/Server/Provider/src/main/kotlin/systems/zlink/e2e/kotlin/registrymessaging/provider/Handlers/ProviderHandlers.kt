package Handlers


import systems.zlink.e2e.kotlin.registrymessaging.provider.Handlers
import systems.zlink.e2e.kotlin.registrymessaging.provider.Infrastructure
import java.util.logging.Handler
import java.util.logging.LogRecord
import java.util.logging.Logger
import java.security.MessageDigest
import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionStage
import java.util.HexFormat
import systems.zlink.e2e.kotlin.registrymessaging.provider.Infrastructure.EvidenceStore
import systems.zlink.e2e.kotlin.registrymessaging.shared.PayloadRes
import systems.zlink.e2e.kotlin.registrymessaging.shared.PayloadReq
import systems.zlink.e2e.kotlin.registrymessaging.shared.ProfileMsg
import systems.zlink.e2e.kotlin.registrymessaging.shared.ProfileRes
import systems.zlink.e2e.kotlin.registrymessaging.shared.ProfileReq
import systems.zlink.e2e.kotlin.registrymessaging.shared.Contracts
import systems.zlink.e2e.kotlin.registrymessaging.shared.ScenarioRoutePingReq
import systems.zlink.e2e.kotlin.registrymessaging.shared.ScenarioRoutePingRes
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.channels.ZLinkRouteMessageContext
import systems.zlink.framework.handlers.ZLinkHandlerGroup
import systems.zlink.framework.kotlin.ZLinkSuspendingRequestHandler
import systems.zlink.framework.kotlin.ZLinkSuspendingRouteRequestHandler
import systems.zlink.framework.kotlin.ZLinkSuspendingSendHandler
import kotlinx.coroutines.delay

@ZLinkHandlerGroup(Contracts.CHANNEL_HANDLER_GROUP)
class ProfileRequestHandler(
    private val evidence: EvidenceStore,
) : ZLinkSuspendingRequestHandler<ProfileReq, ProfileRes> {
    override suspend fun handle(request: ProfileReq, context: ZLinkMessageContext): ProfileRes {
        if (request.value == "slow") {
            delay(1000)
        }
        evidence.add(
            "profile-request|rid=${evidence.rid}|instance=${evidence.instanceId}" +
                "|value=${request.value}|packet=${context.packetName()}",
        )
        return ProfileRes("profile:${request.value}", evidence.rid, evidence.instanceId)
    }
}

@ZLinkHandlerGroup(Contracts.CHANNEL_HANDLER_GROUP)
class ProfileCommandHandler(
    private val evidence: EvidenceStore,
) : ZLinkSuspendingSendHandler<ProfileMsg> {
    override suspend fun handle(message: ProfileMsg, context: ZLinkMessageContext) {
        if (message.commandId.startsWith("slow")) {
            delay(1000)
        }
        evidence.add("profile-command|rid=${evidence.rid}|command=${message.commandId}|packet=${context.packetName()}")
    }
}

@ZLinkHandlerGroup(Contracts.CHANNEL_HANDLER_GROUP)
class PayloadRequestHandler(
    private val evidence: EvidenceStore,
) : ZLinkSuspendingRequestHandler<PayloadReq, PayloadRes> {
    override suspend fun handle(request: PayloadReq, context: ZLinkMessageContext): PayloadRes {
        val digest = MessageDigest.getInstance("SHA-256").digest(request.payload.toByteArray(Charsets.UTF_8))
        val hash = HexFormat.of().formatHex(digest).uppercase()
        evidence.add(
            "payload-request|rid=${evidence.rid}|marker=${request.marker}" +
                "|length=${request.payload.length}|sha256=$hash|packet=${context.packetName()}",
        )
        return PayloadRes(request.marker, request.payload.length, hash)
    }
}

@ZLinkHandlerGroup(Contracts.ROUTE_HANDLER_GROUP)
class RoutePayloadRequestHandler(
    private val evidence: EvidenceStore,
) : ZLinkSuspendingRouteRequestHandler<PayloadReq, PayloadRes> {
    override suspend fun handle(request: PayloadReq, context: ZLinkRouteMessageContext): PayloadRes {
        val digest = MessageDigest.getInstance("SHA-256").digest(request.payload.toByteArray(Charsets.UTF_8))
        val hash = HexFormat.of().formatHex(digest).uppercase()
        evidence.add(
            "payload-request|rid=${evidence.rid}|marker=${request.marker}" +
                "|length=${request.payload.length}|sha256=$hash|packet=${context.packetName()}",
        )
        return PayloadRes(request.marker, request.payload.length, hash)
    }
}

@ZLinkHandlerGroup(Contracts.ROUTE_HANDLER_GROUP)
class RoutePingHandler(
    private val evidence: EvidenceStore,
) : ZLinkSuspendingRouteRequestHandler<ScenarioRoutePingReq, ScenarioRoutePingRes> {
    override suspend fun handle(request: ScenarioRoutePingReq, context: ZLinkRouteMessageContext): ScenarioRoutePingRes {
        val source = context.sourceNodeRid().toString()
        evidence.add("route-request|rid=${evidence.rid}|source=$source|value=${request.value}")
        return ScenarioRoutePingRes("route:${request.value}", evidence.rid, source)
    }
}

class EvidenceDispatchErrorHandler(
    private val evidence: EvidenceStore,
) : Handler() {
    fun install() = Logger.getLogger(LOGGER_NAME).addHandler(this)

    override fun publish(record: LogRecord) {
        val fields = diagnosticsFields(record.message) ?: return
        if (fields["outcome"] != "ERROR") return
        evidence.add(
            "dispatch-error" +
                "|surface=${fields["surface"]}" +
                "|kind=${fields["kind"]}" +
                "|reason=${fields["reason"]}" +
                "|action=${fields["action"]}" +
                "|packet=${fields["packet"] ?: "<null>"}" +
                "|channel=${fields["channel"] ?: "<null>"}",
        )
    }

    override fun flush() = Unit
    override fun close() = Logger.getLogger(LOGGER_NAME).removeHandler(this)

    private fun diagnosticsFields(message: String?): Map<String, String>? {
        if (message?.startsWith("message flow ") != true) return null
        return message.split(' ').mapNotNull { token ->
            val separator = token.indexOf('=')
            if (separator > 0) token.substring(0, separator) to token.substring(separator + 1) else null
        }.toMap()
    }

    companion object {
        private const val LOGGER_NAME = "systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer"
    }
}
