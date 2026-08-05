package systems.zlink.e2e.kotlin.spotservice.session.handlers

import systems.zlink.contracts.core.RoutingId
import systems.zlink.e2e.kotlin.spotservice.Contracts
import systems.zlink.e2e.kotlin.spotservice.ScenarioState
import systems.zlink.framework.actors.ActorRef
import systems.zlink.framework.channels.ZLinkRouteClient
import systems.zlink.framework.streams.ZLinkSessionContext
import systems.zlink.framework.streams.ZLinkSessionDispatchContext
import kotlinx.coroutines.future.await
import systems.zlink.framework.kotlin.ZLinkSuspendingTypedSessionPacketHandler

class RemoteActorAuthHandler(
    private val routes: ZLinkRouteClient,
    private val evidence: ScenarioState
) : ZLinkSuspendingTypedSessionPacketHandler<ZLinkSessionContext, Contracts.ActorRemoteAuthReq> {
    override fun packetName(): String = "ActorRemoteAuthReq"

    override fun messageType(): Class<Contracts.ActorRemoteAuthReq> = Contracts.ActorRemoteAuthReq::class.java

    override suspend fun handle(
        context: ZLinkSessionContext,
        dispatch: ZLinkSessionDispatchContext,
        request: Contracts.ActorRemoteAuthReq
    ) {
        val ensured = routes.requestToNode(
            Contracts.SPOT_MESH,
            RoutingId.from(request.nodeRid),
            Contracts.EnsureActorReq(request.actorId, request.profile)
        )
            .submit(Contracts.EnsureActorRes::class.java).await()
        val bound = try {
            context.actors()
                .bind(
                    ActorRef(
                        ensured.actorId,
                        ensured.generation,
                        Contracts.SPOT_MESH,
                        RoutingId.from(ensured.nodeRid),
                    )
                )
                .await()
        } catch (error: Throwable) {
            evidence.record("RemoteActorSessionBindFailed", "session", error.javaClass.simpleName)
            throw error
        }
        evidence.record("RemoteActorSessionBound", "session", request.actorId + "/" + ensured.nodeRid)
        context.client()
            .reply(
                Contracts.ActorAuthRes(
                    bound.actorId(),
                    bound.ref().nodeRid().toString(),
                    context.actors().bound().size,
                    request.profile.displayName,
                    request.profile.level,
                    request.profile.tags
                )
            )
            .submit()
    }
}
