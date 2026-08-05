package systems.zlink.samples.kotlin.deliverydispatch.server.couriersession.sessions

import systems.zlink.framework.actors.ActorRef
import systems.zlink.framework.actors.ActorRefSnapshot
import systems.zlink.framework.actors.ZLinkActorCreateResult
import systems.zlink.framework.actors.ZLinkActorManager
import systems.zlink.framework.kotlin.await
import systems.zlink.framework.kotlin.kotlin
import systems.zlink.framework.kotlin.ZLinkSuspendingSession
import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.framework.streams.ZLinkSessionContext
import systems.zlink.framework.streams.ZLinkSessionDispatchContext
import systems.zlink.framework.streams.ZLinkSessionPacketDispatcher
import systems.zlink.framework.streams.ZLinkStreamError
import systems.zlink.samples.kotlin.deliverydispatch.server.configuration.SampleNames
import systems.zlink.samples.kotlin.deliverydispatch.shared.contracts.BindCourierSessionReq
import systems.zlink.samples.kotlin.deliverydispatch.shared.contracts.BindCourierSessionRes
import systems.zlink.samples.kotlin.deliverydispatch.shared.contracts.CourierDecisionMsg
import systems.zlink.samples.kotlin.deliverydispatch.shared.contracts.EnsureCourierActorReq

class CourierSession(
    private val sessionContext: ZLinkSessionContext,
    private val handlers: ZLinkSessionPacketDispatcher<ZLinkSessionContext>,
    private val actors: ZLinkActorManager,
) : ZLinkSuspendingSession() {
    override fun context(): ZLinkSessionContext = sessionContext

    override suspend fun onConnectedSuspending() {
    }

    override suspend fun onDisconnectedSuspending() {
        for (actor in sessionContext.actors().bound()) actor.notifyDisconnected().await()
    }

    override suspend fun onErrorSuspending(error: ZLinkStreamError) {
    }

    override suspend fun onDispatchSuspending(dispatch: ZLinkSessionDispatchContext, payload: ZLinkMessage) {
        if (dispatch.packetName() == "BindCourierSessionReq") {
            handleBindCourierSessionReq(dispatch, payload)
            return
        }
        val handled = handlers.tryHandle(sessionContext, dispatch, payload).await()
        if (handled) {
            return
        }
        val decision = payload.decode(CourierDecisionMsg::class.java)
        val actor = sessionContext.actors().find(decision.courierId)
            .orElseThrow { IllegalStateException("Courier actor is not bound: ${decision.courierId}") }
        actor.relay(dispatch, payload).await()
    }

    private suspend fun handleBindCourierSessionReq(dispatch: ZLinkSessionDispatchContext, payload: ZLinkMessage) {
        val request = payload.decode(BindCourierSessionReq::class.java)
        val actorRef = findOrEnsureActor(request.courierId)
        val actor = sessionContext.actors().find(actorRef.actorId).orElse(null)
            ?: sessionContext.actors().bind(actorRef).await()
        val snapshot = ActorRefSnapshot.from(actorRef)
        actor.relay(
                dispatch,
                ZLinkMessage.of(
                    BindCourierSessionReq(
                        courierId = request.courierId,
                        actor = snapshot,
                        sessionRoute = sessionContext.sessionId(),
                    ),
                ),
            ).await()
        sessionContext.client()
            .reply(BindCourierSessionRes(request.courierId, snapshot, sessionContext.sessionId()))
            .submit()
    }

    private suspend fun findOrEnsureActor(courierId: String): ActorRef =
        when (val result = actors.kotlin().getOrCreate(
            courierId,
            SampleNames.CourierActorType,
        ).request(EnsureCourierActorReq(courierId)).await()) {
            is ZLinkActorCreateResult.Existing -> result.actor
            is ZLinkActorCreateResult.Created -> result.actor
            is ZLinkActorCreateResult.Rejected ->
                throw IllegalStateException("Courier Actor creation was rejected.")
        }
}
