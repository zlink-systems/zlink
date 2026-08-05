package systems.zlink.e2e.kotlin.registrationcodec.main.handlers

import org.springframework.beans.factory.ObjectProvider
import systems.zlink.e2e.kotlin.registrationcodec.Contracts
import systems.zlink.e2e.kotlin.registrationcodec.DiLifecycleRes
import systems.zlink.e2e.kotlin.registrationcodec.DiLifecycleReq
import systems.zlink.e2e.kotlin.registrationcodec.EchoAttrMsg
import systems.zlink.e2e.kotlin.registrationcodec.EchoAttrReq
import systems.zlink.e2e.kotlin.registrationcodec.EchoAttrRes
import systems.zlink.e2e.kotlin.registrationcodec.EchoAutoMsg
import systems.zlink.e2e.kotlin.registrationcodec.EchoAutoReq
import systems.zlink.e2e.kotlin.registrationcodec.EchoAutoRes
import systems.zlink.e2e.kotlin.registrationcodec.EchoManualMsg
import systems.zlink.e2e.kotlin.registrationcodec.EchoManualReq
import systems.zlink.e2e.kotlin.registrationcodec.EchoManualRes
import systems.zlink.e2e.kotlin.registrationcodec.main.infrastructure.DiScopedDependency
import systems.zlink.e2e.kotlin.registrationcodec.main.infrastructure.DiSingletonDependency
import systems.zlink.e2e.kotlin.registrationcodec.main.infrastructure.ScenarioState
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.handlers.ZLinkHandlerGroup
import systems.zlink.framework.handlers.ZLinkRequest
import systems.zlink.framework.handlers.ZLinkSend
import systems.zlink.framework.kotlin.ZLinkSuspendingRequestHandler
import systems.zlink.framework.kotlin.ZLinkSuspendingSendHandler

@ZLinkHandlerGroup(Contracts.AUTO_GROUP)
class AutoRequestHandler(
    private val state: ScenarioState,
) : ZLinkSuspendingRequestHandler<EchoAutoReq, EchoAutoRes> {
    override suspend fun handle(request: EchoAutoReq, context: ZLinkMessageContext): EchoAutoRes {
        state.record("Request", "EchoAutoReq", request.value)
        return EchoAutoRes("echo:${request.value}", "auto")
    }
}

@ZLinkHandlerGroup(Contracts.AUTO_GROUP)
class AutoSendHandler(
    private val state: ScenarioState,
) : ZLinkSuspendingSendHandler<EchoAutoMsg> {
    override suspend fun handle(message: EchoAutoMsg, context: ZLinkMessageContext) {
        state.record("Send", "EchoAutoMsg", message.value)
    }
}

@ZLinkHandlerGroup(Contracts.ATTR_GROUP)
class AttrEchoHandler(
    private val state: ScenarioState,
) {
    @ZLinkRequest(packetName = "EchoAttrReq")
    suspend fun request(request: EchoAttrReq, context: ZLinkMessageContext): EchoAttrRes {
        state.record("Request", "EchoAttrReq", request.value)
        return EchoAttrRes("echo:${request.value}", "attr")
    }

    @ZLinkSend(packetName = "EchoAttrMsg")
    suspend fun send(message: EchoAttrMsg, context: ZLinkMessageContext) {
        state.record("Send", "EchoAttrMsg", message.value)
    }
}

@ZLinkHandlerGroup(Contracts.MANUAL_GROUP)
class ManualRequestHandler(
    private val state: ScenarioState,
) : ZLinkSuspendingRequestHandler<EchoManualReq, EchoManualRes> {
    override suspend fun handle(request: EchoManualReq, context: ZLinkMessageContext): EchoManualRes {
        state.record("Request", context.packetName(), request.value)
        return EchoManualRes("echo:${request.value}", "manual")
    }
}

@ZLinkHandlerGroup(Contracts.MANUAL_GROUP)
class ManualSendHandler(
    private val state: ScenarioState,
) : ZLinkSuspendingSendHandler<EchoManualMsg> {
    override suspend fun handle(message: EchoManualMsg, context: ZLinkMessageContext) {
        state.record("Send", context.packetName(), message.value)
    }
}

@ZLinkHandlerGroup(Contracts.MANUAL_GROUP)
class DiLifecycleRequestHandler(
    private val scoped: ObjectProvider<DiScopedDependency>,
    private val singleton: DiSingletonDependency,
    private val state: ScenarioState,
) : ZLinkSuspendingRequestHandler<DiLifecycleReq, DiLifecycleRes> {
    override suspend fun handle(request: DiLifecycleReq, context: ZLinkMessageContext): DiLifecycleRes {
        val scopedId = scoped.getObject().use { dependency ->
            state.record(
                "DI",
                context.packetName(),
                "${dependency.id}:${singleton.id}:${request.value}",
            )
            dependency.id
        }
        return DiLifecycleRes(
            "echo:${request.value}",
            scopedId,
            singleton.id,
            state.diDisposeCount(),
        )
    }
}
