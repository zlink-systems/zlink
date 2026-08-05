package systems.zlink.e2e.kotlin.registrymessaging.provider.Handlers

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
import systems.zlink.framework.configuration.ZLinkMessageFlowEvent
import systems.zlink.framework.configuration.ZLinkMessageFlowObserver
import systems.zlink.framework.configuration.ZLinkMessageFlowOutcome
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
class RoutePingHandler(
    private val evidence: EvidenceStore,
) : ZLinkSuspendingRouteRequestHandler<ScenarioRoutePingReq, ScenarioRoutePingRes> {
    override suspend fun handle(request: ScenarioRoutePingReq, context: ZLinkRouteMessageContext): ScenarioRoutePingRes {
        val source = context.sourceNodeRid().toString()
        evidence.add("route-request|rid=${evidence.rid}|source=$source|value=${request.value}")
        return ScenarioRoutePingRes("route:${request.value}", evidence.rid, source)
    }
}

class EvidenceDispatchErrorObserver(
    private val evidence: EvidenceStore,
) : ZLinkMessageFlowObserver {
    override fun onMessageFlow(flow: ZLinkMessageFlowEvent): CompletionStage<Void> {
        if (flow.outcome() != ZLinkMessageFlowOutcome.ERROR) {
            return CompletableFuture.completedFuture(null)
        }
        evidence.add(
            "dispatch-error" +
                "|surface=${flow.surface()}" +
                "|kind=${flow.messageKind()}" +
                "|reason=${flow.errorReason()}" +
                "|action=${flow.errorAction()}" +
                "|packet=${flow.packetName() ?: "<null>"}" +
                "|channel=${flow.channelName() ?: "<null>"}",
        )
        return CompletableFuture.completedFuture(null)
    }
}
