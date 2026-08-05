package systems.zlink.e2e.kotlin.pubsub.subscriber

import systems.zlink.e2e.kotlin.pubsub.shared.Contracts
import systems.zlink.e2e.kotlin.pubsub.shared.EventMsg
import systems.zlink.framework.channels.ZLinkPublishMessageContext
import systems.zlink.framework.handlers.ZLinkHandlerGroup
import systems.zlink.framework.kotlin.ZLinkSuspendingPublishHandler

@ZLinkHandlerGroup(Contracts.HANDLER_GROUP)
class EventMsgHandler(
    private val state: EvidenceStore,
) : ZLinkSuspendingPublishHandler<EventMsg> {
    override suspend fun handle(
        message: EventMsg,
        context: ZLinkPublishMessageContext,
    ) {
        if (!state.accepts(context.topic())) {
            return
        }
        state.delayIfConfigured(message.scenario)
        state.record(
            "EventMsg",
            context.topic(),
            message.scenario,
            message.sequence,
            message.value,
        )
    }
}
