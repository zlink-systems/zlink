package systems.zlink.e2e.kotlin.observabilityops.a5.server

import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.channels.ZLinkRequestHandler
import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionStage

class ProbeHandler : ZLinkRequestHandler<Contracts.ProbeReq, Contracts.ProbeRes> {
    override fun handle(
        request: Contracts.ProbeReq,
        context: ZLinkMessageContext,
    ): CompletionStage<Contracts.ProbeRes> = if (request.fail) {
        CompletableFuture.failedFuture(
            IllegalStateException("observability probe handler failure"),
        )
    } else {
        CompletableFuture.completedFuture(Contracts.ProbeRes(request.value))
    }
}
