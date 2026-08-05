package systems.zlink.e2e.kotlin.runtimemonitoring.service

import systems.zlink.e2e.kotlin.runtimemonitoring.Contracts
import systems.zlink.e2e.kotlin.runtimemonitoring.Env
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.kotlin.ZLinkSuspendingRequestHandler
import systems.zlink.framework.handlers.ZLinkHandlerGroup

@ZLinkHandlerGroup(Contracts.HANDLER_GROUP)
class WorkRequestHandler : ZLinkSuspendingRequestHandler<Contracts.WorkReq, Contracts.WorkRes> {
    override suspend fun handle(
        request: Contracts.WorkReq,
        context: ZLinkMessageContext,
    ): Contracts.WorkRes {
        return Contracts.WorkRes(
            "work:${request.value}",
            Env.get("e2e.rid", "svc-a"),
        )
    }
}
