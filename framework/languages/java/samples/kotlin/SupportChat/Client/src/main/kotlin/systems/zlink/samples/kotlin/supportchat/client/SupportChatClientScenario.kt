package systems.zlink.samples.kotlin.supportchat.client

import java.time.Duration
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.async
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.future.await
import systems.zlink.framework.kotlin.ZLinkKotlinStreamAssert
import systems.zlink.framework.kotlin.kotlin
import systems.zlink.samples.kotlin.supportchat.server.configuration.ConversationStatuses
import systems.zlink.samples.kotlin.supportchat.server.configuration.SampleNames
import systems.zlink.samples.kotlin.supportchat.server.configuration.SampleTimings
import systems.zlink.samples.kotlin.supportchat.server.configuration.SupportChatRoles
import systems.zlink.samples.kotlin.supportchat.shared.contracts.AuthenticateReq
import systems.zlink.samples.kotlin.supportchat.shared.contracts.AuthenticateRes
import systems.zlink.samples.kotlin.supportchat.shared.contracts.ChatMessageNotify
import systems.zlink.samples.kotlin.supportchat.shared.contracts.CloseConversationReq
import systems.zlink.samples.kotlin.supportchat.shared.contracts.CloseConversationRes
import systems.zlink.samples.kotlin.supportchat.shared.contracts.ConversationAssignedNotify
import systems.zlink.samples.kotlin.supportchat.shared.contracts.ConversationClosedNotify
import systems.zlink.samples.kotlin.supportchat.shared.contracts.ConversationIdleNotify
import systems.zlink.samples.kotlin.supportchat.shared.contracts.JoinConversationReq
import systems.zlink.samples.kotlin.supportchat.shared.contracts.JoinConversationRes
import systems.zlink.samples.kotlin.supportchat.shared.contracts.OpenConversationReq
import systems.zlink.samples.kotlin.supportchat.shared.contracts.OpenConversationRes
import systems.zlink.samples.kotlin.supportchat.shared.contracts.ParticipantJoinedNotify
import systems.zlink.samples.kotlin.supportchat.shared.contracts.SendChatMessageReq
import systems.zlink.samples.kotlin.supportchat.shared.contracts.SendChatMessageRes
import systems.zlink.samples.kotlin.supportchat.shared.contracts.SetAgentAvailableReq
import systems.zlink.samples.kotlin.supportchat.shared.contracts.SetAgentAvailableRes
import systems.zlink.samples.kotlin.supportchat.shared.contracts.SetTypingReq
import systems.zlink.samples.kotlin.supportchat.shared.contracts.TypingChangedNotify
import systems.zlink.stream.connector.ZLinkStreamConnector

class SupportChatClientScenario {
    suspend fun run(
        agent: ZLinkStreamConnector,
        customer1: ZLinkStreamConnector,
        customer2: ZLinkStreamConnector,
        reconnectingAgent: ZLinkStreamConnector,
        reconnectingCustomer: ZLinkStreamConnector,
        waitingCustomer: ZLinkStreamConnector,
    ) = coroutineScope {
        agent.connect().submit().await()
        val agentAuth = agent.request(AuthenticateReq("agent-1")).submit(AuthenticateRes::class.java).await()
        ensure(agentAuth.actorId == "agent-1")
        ensure(agentAuth.role == SupportChatRoles.Agent)
        ensure(agent.request(SetAgentAvailableReq(true)).submit(SetAgentAvailableRes::class.java).await().isAvailable)

        customer1.connect().submit().await()
        ensure(customer1.request(AuthenticateReq("customer-1")).submit(AuthenticateRes::class.java).await().actorId == "customer-1")
        val assigned1ForAgent = async { agent.waitFor(ConversationAssignedNotify::class.java).submit(ConversationAssignedNotify::class.java).await() }
        val opened1 = customer1.request(OpenConversationReq("checkout payment failed")).submit(OpenConversationRes::class.java).await()
        val cid1 = opened1.conversationId
        ensure(opened1.state.status == ConversationStatuses.WaitingForAgent)
        ensure(assigned1ForAgent.await().payload().conversationId == cid1)

        val joined1ForCustomer = async { customer1.waitFor(ParticipantJoinedNotify::class.java).submit(ParticipantJoinedNotify::class.java).await() }
        var agentRoom1 = ConversationClient(agent, cid1)
        var customerRoom1 = ConversationClient(customer1, cid1)
        val agentJoin1 = agentRoom1.join()
        ensure(agentJoin1.scheduled)
        ensure(agentJoin1.state.status == ConversationStatuses.WaitingForAgent)
        val joined1 = joined1ForCustomer.await().payload()
        ensure(joined1.conversationId == cid1)
        ensure(joined1.actorId == "agent-1")
        ensure(joined1.state.status == ConversationStatuses.Active)

        val greeting1ForCustomer = async { customer1.waitFor(ChatMessageNotify::class.java).submit(ChatMessageNotify::class.java).await() }
        val greet1 = agentRoom1.sendChat("How can I help?")
        ensure(greet1.message.messageSeq == 1L)
        val greeting1 = greeting1ForCustomer.await().payload()
        ensure(greeting1.conversationId == cid1)
        ensure(greeting1.message.messageSeq == 1L)

        val reply1ForAgent = async { agent.waitFor(ChatMessageNotify::class.java).submit(ChatMessageNotify::class.java).await() }
        val reply1 = customerRoom1.sendChat("Payment keeps failing.")
        ensure(reply1.message.messageSeq == 2L)
        val reply1Push = reply1ForAgent.await().payload()
        ensure(reply1Push.conversationId == cid1)
        ensure(reply1Push.message.messageSeq == 2L)

        customer2.connect().submit().await()
        ensure(customer2.request(AuthenticateReq("customer-2")).submit(AuthenticateRes::class.java).await().actorId == "customer-2")
        val assigned2ForAgent = async { agent.waitFor(ConversationAssignedNotify::class.java).submit(ConversationAssignedNotify::class.java).await() }
        val opened2 = customer2.request(OpenConversationReq("cannot log in")).submit(OpenConversationRes::class.java).await()
        val cid2 = opened2.conversationId
        ensure(cid2 != cid1)
        ensure(opened2.state.status == ConversationStatuses.WaitingForAgent)
        ensure(assigned2ForAgent.await().payload().conversationId == cid2)

        val joined2ForCustomer = async { customer2.waitFor(ParticipantJoinedNotify::class.java).submit(ParticipantJoinedNotify::class.java).await() }
        val agentRoom2 = ConversationClient(agent, cid2)
        val customerRoom2 = ConversationClient(customer2, cid2)
        val agentJoin2 = agentRoom2.join()
        ensure(agentJoin2.scheduled)
        ensure(agentJoin2.state.status == ConversationStatuses.WaitingForAgent)
        ensure(joined2ForCustomer.await().payload().conversationId == cid2)

        val greeting2ForCustomer = async { customer2.waitFor(ChatMessageNotify::class.java).submit(ChatMessageNotify::class.java).await() }
        val greet2 = agentRoom2.sendChat("Let me check your account.")
        ensure(greet2.message.messageSeq == 1L)
        val greeting2 = greeting2ForCustomer.await().payload()
        ensure(greeting2.conversationId == cid2)
        ensure(greeting2.message.messageSeq == 1L)

        val typingForCustomer1 = async { customer1.waitFor(TypingChangedNotify::class.java).submit(TypingChangedNotify::class.java).await() }
        agentRoom1.sendTyping(true)
        val typing1 = typingForCustomer1.await().payload()
        ensure(typing1.conversationId == cid1)
        ensure(typing1.actorId == "agent-1")
        ensure(typing1.isTyping)

        customer1.close().submit().await()
        reconnectingCustomer.connect().submit().await()
        ensure(reconnectingCustomer.request(AuthenticateReq("customer-1")).submit(AuthenticateRes::class.java).await().actorId == "customer-1")
        customerRoom1 = ConversationClient(reconnectingCustomer, cid1)
        val customerRejoin1 = customerRoom1.join()
        ensure(!customerRejoin1.scheduled)
        ensure(customerRejoin1.state.subject == "checkout payment failed")
        ensure(customerRejoin1.state.status == ConversationStatuses.Active)
        ensure(customerRejoin1.state.lastMessageSeq == 2L)

        agent.close().submit().await()
        reconnectingAgent.connect().submit().await()
        ensure(reconnectingAgent.request(AuthenticateReq("agent-1")).submit(AuthenticateRes::class.java).await().actorId == "agent-1")
        ensure(reconnectingAgent.request(SetAgentAvailableReq(true)).submit(SetAgentAvailableRes::class.java).await().isAvailable)
        val reconnectedRoom1 = ConversationClient(reconnectingAgent, cid1)
        val reconnectedRoom2 = ConversationClient(reconnectingAgent, cid2)
        val agentRejoin1 = reconnectedRoom1.join()
        val agentRejoin2 = reconnectedRoom2.join()
        ensure(!agentRejoin1.scheduled)
        ensure(!agentRejoin2.scheduled)
        ensure(agentRejoin1.state.subject == "checkout payment failed")
        ensure(agentRejoin2.state.subject == "cannot log in")

        val idleTimeout = SampleTimings.IdleTimeout
            .plus(SampleTimings.CloseGraceTimeout)
            .plus(SampleTimings.RequestTimeout)
        val idle1ForCustomer = reconnectingCustomer.waitFor(ConversationIdleNotify::class.java)
            .timeout(idleTimeout)
            .submit(ConversationIdleNotify::class.java)
        val idle1ForAgent = reconnectingAgent.waitFor(ConversationIdleNotify::class.java)
            .where(ConversationIdleNotify::class.java) { it.payload().conversationId == cid1 }
            .timeout(idleTimeout)
            .submit(ConversationIdleNotify::class.java)
        val closed1ForCustomer = reconnectingCustomer.waitFor(ConversationClosedNotify::class.java)
            .timeout(idleTimeout)
            .submit(ConversationClosedNotify::class.java)
        val closed1ForAgent = reconnectingAgent.waitFor(ConversationClosedNotify::class.java)
            .where(ConversationClosedNotify::class.java) { it.payload().conversationId == cid1 }
            .timeout(idleTimeout)
            .submit(ConversationClosedNotify::class.java)
        val closed2ForAgent = reconnectingAgent.waitFor(ConversationClosedNotify::class.java)
            .where(ConversationClosedNotify::class.java) { it.payload().conversationId == cid2 }
            .timeout(idleTimeout)
            .submit(ConversationClosedNotify::class.java)
        val closed2 = customerRoom2.close("resolved")
        ensure(closed2.state.status == ConversationStatuses.Closed)
        val closed2Agent = closed2ForAgent.await().payload()
        ensure(closed2Agent.conversationId == cid2)
        ensure(closed2Agent.state.status == ConversationStatuses.Closed)

        ZLinkKotlinStreamAssert.expectFailure {
            customerRoom2.close("again")
        }

        ensure(idle1ForCustomer.await().payload().state.status == ConversationStatuses.WaitingForClose)
        val idle1Agent = idle1ForAgent.await().payload()
        ensure(idle1Agent.conversationId == cid1)
        ensure(idle1Agent.state.status == ConversationStatuses.WaitingForClose)
        ensure(closed1ForCustomer.await().payload().state.status == ConversationStatuses.Closed)
        val closed1Agent = closed1ForAgent.await().payload()
        ensure(closed1Agent.conversationId == cid1)
        ensure(closed1Agent.state.status == ConversationStatuses.Closed)

        ZLinkKotlinStreamAssert.expectFailure {
            customerRoom1.sendChat("are you there?")
        }
        val closedTypingForAgent = async(start = CoroutineStart.UNDISPATCHED) {
            reconnectingAgent.kotlin().expectNone<TypingChangedNotify>(TypingChangedNotify::class.java.simpleName)
                .within(Duration.ofMillis(500))
                .await()
        }
        customerRoom1.sendTyping(true)
        closedTypingForAgent.await()
        println("supportchat-closed-typing-ignore=verified")

        ensure(!reconnectingAgent.request(SetAgentAvailableReq(false)).submit(SetAgentAvailableRes::class.java).await().isAvailable)
        waitingCustomer.connect().submit().await()
        ensure(waitingCustomer.request(AuthenticateReq("customer-3")).submit(AuthenticateRes::class.java).await().actorId == "customer-3")
        ZLinkKotlinStreamAssert.expectFailure {
            waitingCustomer.request(SetAgentAvailableReq(true)).submit(SetAgentAvailableRes::class.java).await()
        }
        val noClosedNotification = async(start = CoroutineStart.UNDISPATCHED) {
            waitingCustomer.kotlin().expectNone<ConversationClosedNotify>(ConversationClosedNotify::class.java.simpleName)
                .within(Duration.ofMillis(500))
                .await()
        }
        val noAgentOpen = waitingCustomer.request(OpenConversationReq("agent unavailable")).submit(OpenConversationRes::class.java).await()
        ensure(noAgentOpen.state.status == ConversationStatuses.WaitingForAgent)
        ensure(noAgentOpen.state.subject == "agent unavailable")
        noClosedNotification.await()
    }

    private class ConversationClient(
        private val connector: ZLinkStreamConnector,
        private val conversationId: String,
    ) {
        suspend fun join(): JoinConversationRes =
            connector.request(JoinConversationReq())
                .metadata(SampleNames.ConversationIdMetadataKey, conversationId)
                .submit(JoinConversationRes::class.java)
                .await()

        suspend fun sendChat(text: String): SendChatMessageRes =
            connector.request(SendChatMessageReq(text = text))
                .metadata(SampleNames.ConversationIdMetadataKey, conversationId)
                .submit(SendChatMessageRes::class.java)
                .await()

        fun sendTyping(isTyping: Boolean) {
            connector.send(SetTypingReq(isTyping = isTyping))
                .metadata(SampleNames.ConversationIdMetadataKey, conversationId)
                .submit()
        }

        suspend fun close(reason: String?): CloseConversationRes =
            connector.request(CloseConversationReq(reason = reason))
                .metadata(SampleNames.ConversationIdMetadataKey, conversationId)
                .submit(CloseConversationRes::class.java)
                .await()
    }
}

private fun ensure(condition: Boolean) {
    if (!condition) {
        throw IllegalStateException("Ensure failed")
    }
}
