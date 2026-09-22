package systems.zlink.samples.kotlin.supportchat.server.session.sessions

import org.slf4j.LoggerFactory
import systems.zlink.framework.channels.ZLinkClient
import systems.zlink.framework.kotlin.ZLinkSuspendingSession
import systems.zlink.framework.kotlin.await
import systems.zlink.framework.kotlin.bindOrGetActor
import systems.zlink.framework.kotlin.decode
import systems.zlink.framework.kotlin.kotlin
import systems.zlink.framework.kotlin.requestToChannel
import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.framework.streams.ZLinkSessionActor
import systems.zlink.framework.streams.ZLinkSessionContext
import systems.zlink.framework.streams.ZLinkSessionDispatchContext
import systems.zlink.framework.streams.ZLinkStreamError
import systems.zlink.samples.kotlin.supportchat.server.configuration.SampleNames
import systems.zlink.samples.kotlin.supportchat.server.configuration.SampleTimings
import systems.zlink.samples.kotlin.supportchat.server.configuration.SupportChatRoles
import systems.zlink.samples.kotlin.supportchat.shared.contracts.AuthenticateReq
import systems.zlink.samples.kotlin.supportchat.shared.contracts.AuthenticateRes
import systems.zlink.samples.kotlin.supportchat.shared.contracts.AuthenticateUserReq
import systems.zlink.samples.kotlin.supportchat.shared.contracts.AuthenticateUserRes
import systems.zlink.samples.kotlin.supportchat.shared.contracts.EnsureAgentConversationReq
import systems.zlink.samples.kotlin.supportchat.shared.contracts.EnsureAgentConversationRes
import systems.zlink.samples.kotlin.supportchat.shared.contracts.EnsureSupportUserActorReq
import systems.zlink.samples.kotlin.supportchat.shared.contracts.EnsureSupportUserActorRes
import systems.zlink.samples.kotlin.supportchat.shared.contracts.JoinConversationRes

class SupportChatSession(
    private val context: ZLinkSessionContext,
    private val channels: ZLinkClient,
) : ZLinkSuspendingSession() {
    private val kotlinChannels = channels.kotlin()
    private val conversationActors = linkedMapOf<String, ZLinkSessionActor>()
    private var identityActor: ZLinkSessionActor? = null
    private var identityActorId: String = ""
    private var identityDisplayName: String = ""
    private var identityRole: String = ""

    override fun context(): ZLinkSessionContext = context

    override suspend fun onDisconnectedSuspending() {
        for (actor in context.actors().bound()) {
            actor.notifyDisconnected().await()
        }
    }

    override suspend fun onErrorSuspending(error: ZLinkStreamError) {}

    // --8<-- [start:doc-sc-session-dispatch]
    override suspend fun onDispatchSuspending(
        dispatch: ZLinkSessionDispatchContext,
        payload: ZLinkMessage,
    ) {
        when (dispatch.packetName()) {
            "AuthenticateReq" -> authenticate(payload.decode<AuthenticateReq>())
            "JoinConversationReq" -> joinConversation(dispatch, payload)
            else -> relayConversationPacket(dispatch, payload)
        }
    }

    // --8<-- [end:doc-sc-session-dispatch]

    private suspend fun authenticate(request: AuthenticateReq) {
        val authenticated =
            kotlinChannels
                .requestToChannel<AuthenticateUserRes>(
                    SampleNames.ApiChannel,
                    AuthenticateUserReq(request.accessToken),
                )
                .timeout(SampleTimings.RequestTimeout)
                .await()
        if (
            !authenticated.accepted ||
                authenticated.actorId.isNullOrBlank() ||
                authenticated.displayName.isNullOrBlank() ||
                authenticated.role.isNullOrBlank()
        ) {
            throw IllegalStateException(
                authenticated.reason ?: "SupportChat authentication failed."
            )
        }
        val actorId =
            authenticated.actorId
                ?: throw IllegalStateException(
                    "SupportChat authentication did not return an actor id."
                )
        val displayName =
            authenticated.displayName
                ?: throw IllegalStateException(
                    "SupportChat authentication did not return a display name."
                )
        val role =
            authenticated.role
                ?: throw IllegalStateException("SupportChat authentication did not return a role.")

        // --8<-- [start:doc-sc-session-auth]
        val ensured =
            kotlinChannels
                .requestToChannel<EnsureSupportUserActorRes>(
                    SampleNames.SupportChannel,
                    EnsureSupportUserActorReq(
                        actorId = actorId,
                        displayName = displayName,
                        role = role,
                        participantId = actorId,
                    ),
                )
                .timeout(SampleTimings.RequestTimeout)
                .await()

        identityActor = context.actors().bindOrGetActor(ensured.actor.toActorRef())
        identityActorId = actorId
        identityDisplayName = displayName
        identityRole = role
        // --8<-- [end:doc-sc-session-auth]
        context.client().kotlin().reply(AuthenticateRes(actorId, displayName, role)).await()
    }

    private suspend fun joinConversation(
        dispatch: ZLinkSessionDispatchContext,
        payload: ZLinkMessage,
    ) {
        if (identityRole == SupportChatRoles.Customer) {
            requireIdentityActor().kotlin().relay(dispatch, payload).await()
            return
        }

        val conversationId = requireConversationId(dispatch)
        val existing = conversationActors[conversationId]
        if (existing != null) {
            existing.kotlin().relay(dispatch, payload).await()
            return
        }

        // --8<-- [start:doc-sc-agent-join]
        val ensured =
            kotlinChannels
                .requestToChannel<EnsureAgentConversationRes>(
                    SampleNames.SupportChannel,
                    EnsureAgentConversationReq(identityActorId, identityDisplayName, conversationId),
                )
                .timeout(SampleTimings.RequestTimeout)
                .await()

        conversationActors[conversationId] =
            context.actors().bindOrGetActor(ensured.actor.toActorRef())
        logger.info(
            "session: agent joined conversation. roster={}, conversation={}",
            identityActorId,
            conversationId,
        )
        context
            .client()
            .kotlin()
            .reply(JoinConversationRes(ensured.scheduled, ensured.state))
            .await()
        // --8<-- [end:doc-sc-agent-join]
    }

    // --8<-- [start:doc-sc-metadata-relay]
    private suspend fun relayConversationPacket(
        dispatch: ZLinkSessionDispatchContext,
        payload: ZLinkMessage,
    ) {
        val conversationId = dispatch.metadata()[SampleNames.ConversationIdMetadataKey]
        val target = conversationId?.let { conversationActors[it] } ?: requireIdentityActor()
        target.kotlin().relay(dispatch, payload).await()
    }

    // --8<-- [end:doc-sc-metadata-relay]

    private fun requireIdentityActor(): ZLinkSessionActor =
        identityActor
            ?: throw IllegalStateException(
                "Client must authenticate before sending conversation packets."
            )

    private fun requireConversationId(dispatch: ZLinkSessionDispatchContext): String =
        dispatch.metadata()[SampleNames.ConversationIdMetadataKey]
            ?: throw IllegalStateException(
                "Conversation packet is missing the ConversationId metadata."
            )

    private companion object {
        private val logger = LoggerFactory.getLogger(SupportChatSession::class.java)
    }
}
