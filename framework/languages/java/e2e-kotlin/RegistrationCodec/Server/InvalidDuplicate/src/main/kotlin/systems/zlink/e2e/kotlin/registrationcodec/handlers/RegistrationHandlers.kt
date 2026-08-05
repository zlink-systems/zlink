package systems.zlink.e2e.kotlin.registrationcodec.handlers

import java.util.concurrent.CompletableFuture
import systems.zlink.e2e.kotlin.registrationcodec.EchoManualReq
import systems.zlink.e2e.kotlin.registrationcodec.EchoManualRes
import systems.zlink.e2e.kotlin.registrationcodec.ScenarioState
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.channels.ZLinkRequestHandler

class ManualRequestHandler(
    private val state: ScenarioState,
) : ZLinkRequestHandler<EchoManualReq, EchoManualRes> {
    override fun handle(
        request: EchoManualReq,
        context: ZLinkMessageContext,
    ): java.util.concurrent.CompletionStage<EchoManualRes> {
        state.record("Request", "DuplicatePacket", request.value)
        return CompletableFuture.completedFuture(EchoManualRes("echo:${request.value}", "manual"))
    }
}
