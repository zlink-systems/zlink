package systems.zlink.e2e.kotlin.spotservice.play.handlers

import systems.zlink.e2e.kotlin.spotservice.Contracts
import systems.zlink.e2e.kotlin.spotservice.ScenarioState
import systems.zlink.e2e.kotlin.spotservice.play.spots.*
import systems.zlink.framework.channels.ZLinkRouteMessageContext
import systems.zlink.framework.kotlin.ZLinkSuspendingRouteRequestHandler

class RoutePingHandler(
    private val state: ScenarioState,
) : ZLinkSuspendingRouteRequestHandler<Contracts.RoutePingReq, Contracts.RoutePingRes> {
    override suspend fun handle(
        request: Contracts.RoutePingReq,
        context: ZLinkRouteMessageContext,
    ): Contracts.RoutePingRes {
        state.record("RoutePingReq", context.sourceNodeRid().toString(), request.value)
        return Contracts.RoutePingRes(
            "route:${request.value}",
            state.nodeRid(),
            context.sourceNodeRid().toString(),
        )
    }
}
