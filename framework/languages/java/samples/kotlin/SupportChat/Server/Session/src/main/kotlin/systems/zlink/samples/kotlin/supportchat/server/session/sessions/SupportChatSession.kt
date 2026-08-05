package systems.zlink.samples.kotlin.supportchat.server.session.sessions

import kotlinx.coroutines.future.await
import org.slf4j.LoggerFactory
import systems.zlink.framework.channels.ZLinkClient
import systems.zlink.framework.kotlin.ZLinkSuspendingSession
import systems.zlink.framework.kotlin.bindOrGetActor
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

    override suspend fun onErrorSuspending(error: ZLinkStreamError) {
    }

    override suspend fun onDispatchSuspending(
        dispatch: ZLinkSessionDispatchContext,
        payload: ZLinkMessage,
    ) {
        when (dispatch.packetName()) {
            "AuthenticateReq" -> authenticate(payload.decode(AuthenticateReq::class.java))
            "JoinConversationReq" -> joinConversation(dispatch, payload)
            else -> relayConversationPacket(dispatch, payload)
        }
    }

    private suspend fun authenticate(request: AuthenticateReq) {
        val authenticated = channels
            .requestToChannel(SampleNames.ApiChannel, AuthenticateUserReq(request.accessToken))
            .timeout(SampleTimings.RequestTimeout)
            .submit(AuthenticateUserRes::class.java)
            .await()
        if (!authenticated.accepted ||
            authenticated.actorId.isNullOrBlank() ||
            authenticated.displayName.isNullOrBlank() ||
            authenticated.role.isNullOrBlank()
        ) {
            throw IllegalStateException(authenticated.reason ?: "SupportChat authentication failed.")
        }
        val actorId = authenticated.actorId
            ?: throw IllegalStateException("SupportChat authentication did not return an actor id.")
        val displayName = authenticated.displayName
            ?: throw IllegalStateException("SupportChat authentication did not return a display name.")
        val role = authenticated.role
            ?: throw IllegalStateException("SupportChat authentication did not return a role.")

        val ensured = channels
            .requestToChannel(
                SampleNames.SupportChannel,
                EnsureSupportUserActorReq(
                    actorId = actorId,
                    displayName = displayName,
                    role = role,
                    participantId = actorId,
                ),
            )
            .timeout(SampleTimings.RequestTimeout)
            .submit(EnsureSupportUserActorRes::class.java)
            .await()

        identityActor = context.actors().bindOrGetActor(ensured.actor.toActorRef())
        identityActorId = actorId
        identityDisplayName = displayName
        identityRole = role
        context.client()
            .reply(AuthenticateRes(actorId, displayName, role))
            .submit()
    }

    private suspend fun joinConversation(
        dispatch: ZLinkSessionDispatchContext,
        payload: ZLinkMessage,
    ) {
        if (identityRole == SupportChatRoles.Customer) {
            requireIdentityActor().relay(dispatch, payload).await()
            return
        }

        val conversationId = requireConversationId(dispatch)
        val existing = conversationActors[conversationId]
        if (existing != null) {
            existing.relay(dispatch, payload).await()
            return
        }

        val ensured = channels
            .requestToChannel(
                SampleNames.SupportChannel,
                EnsureAgentConversationReq(identityActorId, identityDisplayName, conversationId),
            )
            .timeout(SampleTimings.RequestTimeout)
            .submit(EnsureAgentConversationRes::class.java)
            .await()

        conversationActors[conversationId] = context.actors().bindOrGetActor(ensured.actor.toActorRef())
        logger.info(
            "session: agent joined conversation. roster={}, conversation={}",
            identityActorId,
            conversationId,
        )
        context.client().reply(JoinConversationRes(ensured.scheduled, ensured.state)).submit()
    }

    private suspend fun relayConversationPacket(
        dispatch: ZLinkSessionDispatchContext,
        payload: ZLinkMessage,
    ) {
        val conversationId = dispatch.metadata()[SampleNames.ConversationIdMetadataKey]
        val target = conversationId?.let { conversationActors[it] } ?: requireIdentityActor()
        target.relay(dispatch, payload).await()
    }

    private fun requireIdentityActor(): ZLinkSessionActor =
        identityActor ?: throw IllegalStateException(
            "Client must authenticate before sending conversation packets.",
        )

    private fun requireConversationId(dispatch: ZLinkSessionDispatchContext): String =
        dispatch.metadata()[SampleNames.ConversationIdMetadataKey]
            ?: throw IllegalStateException("Conversation packet is missing the ConversationId metadata.")

    private companion object {
        private val logger = LoggerFactory.getLogger(SupportChatSession::class.java)
    }
}
