package systems.zlink.samples.kotlin.supportchat.server.support.infrastructure.zlink.spots.entryspot.handlers

import systems.zlink.framework.kotlin.await
import systems.zlink.framework.kotlin.ZLinkSuspendingEntrySpotActorRequestHandler
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.samples.kotlin.supportchat.server.configuration.SampleNames
import systems.zlink.samples.kotlin.supportchat.server.configuration.SampleTimings
import systems.zlink.samples.kotlin.supportchat.server.configuration.SupportChatRoles
import systems.zlink.samples.kotlin.supportchat.server.support.infrastructure.zlink.actors.SupportUserActor
import systems.zlink.samples.kotlin.supportchat.server.support.infrastructure.zlink.spots.entryspot.SupportEntrySpot
import systems.zlink.samples.kotlin.supportchat.shared.contracts.JoinConversationReq
import systems.zlink.samples.kotlin.supportchat.shared.contracts.JoinConversationRes
import systems.zlink.samples.kotlin.supportchat.shared.contracts.OpenConversationApiReq
import systems.zlink.samples.kotlin.supportchat.shared.contracts.OpenConversationApiRes
import systems.zlink.samples.kotlin.supportchat.shared.contracts.OpenConversationReq
import systems.zlink.samples.kotlin.supportchat.shared.contracts.OpenConversationRes

class OpenConversationActorHandler : ZLinkSuspendingEntrySpotActorRequestHandler<
    SupportEntrySpot,
    SupportUserActor,
    OpenConversationReq,
    OpenConversationRes,
    > {
    override suspend fun handle(
        entrySpot: SupportEntrySpot,
        actor: SupportUserActor,
        context: ZLinkMessageContext,
        request: OpenConversationReq,
    ): OpenConversationRes {
        if (actor.role != SupportChatRoles.Customer) {
            throw IllegalStateException("Only customer actors can open a conversation.")
        }

        val opened = entrySpot.context().outbound()
            .requestToChannel(
                SampleNames.ApiChannel,
                OpenConversationApiReq(
                    actor.participantId,
                    actor.displayName,
                    request.subject,
                ),
            )
            .timeout(SampleTimings.RequestTimeout)
            .submit(OpenConversationApiRes::class.java).await()

        val joined = actor
            .scheduleConversationJoin(
                opened.conversationId,
                request.subject,
                JoinConversationReq(actor.participantId, actor.role, actor.displayName),
            )
        return OpenConversationRes(opened.conversationId, joined.state)
    }
}
