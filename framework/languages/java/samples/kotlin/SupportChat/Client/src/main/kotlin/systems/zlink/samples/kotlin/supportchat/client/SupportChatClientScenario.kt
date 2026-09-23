package systems.zlink.samples.kotlin.supportchat.client

import java.time.Duration
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.async
import kotlinx.coroutines.coroutineScope
import systems.zlink.framework.kotlin.ZLinkKotlinStreamActor
import systems.zlink.framework.kotlin.ZLinkKotlinStreamAssert
import systems.zlink.framework.kotlin.ZLinkKotlinStreamConnector
import systems.zlink.framework.kotlin.request
import systems.zlink.samples.kotlin.supportchat.server.configuration.ConversationStatuses
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
import systems.zlink.samples.kotlin.supportchat.shared.contracts.SetTypingMsg
import systems.zlink.samples.kotlin.supportchat.shared.contracts.TypingChangedNotify

class SupportChatClientScenario {
    suspend fun run(
        agent: ZLinkKotlinStreamConnector,
        customer1: ZLinkKotlinStreamConnector,
        customer2: ZLinkKotlinStreamConnector,
        reconnectingAgent: ZLinkKotlinStreamConnector,
        reconnectingCustomer: ZLinkKotlinStreamConnector,
        waitingCustomer: ZLinkKotlinStreamConnector,
    ) = coroutineScope {
        agent.connect().await()
        val agentAuth = agent.request<AuthenticateRes>(AuthenticateReq("agent-1")).await()
        ensure(agentAuth.actorId == "agent-1")
        ensure(agentAuth.role == SupportChatRoles.Agent)
        ensure(agent.request<SetAgentAvailableRes>(SetAgentAvailableReq(true)).await().isAvailable)

        customer1.connect().await()
        val customer1Auth =
            customer1.request<AuthenticateRes>(AuthenticateReq("customer-1")).await()
        ensure(customer1Auth.actorId == "customer-1")
        val assigned1ForAgent = async { agent.waitFor<ConversationAssignedNotify>().await() }
        val opened1 =
            customer1
                .request<OpenConversationRes>(OpenConversationReq("checkout payment failed"))
                .await()
        val cid1 = opened1.conversationId
        ensure(opened1.state.status == ConversationStatuses.WaitingForAgent)
        ensure(assigned1ForAgent.await().payload().conversationId == cid1)

        val joined1ForCustomer = async { customer1.waitFor<ParticipantJoinedNotify>().await() }
        var agentRoom1 = ConversationClient(agent, cid1, agentAuth)
        var customerRoom1 = ConversationClient(customer1, cid1, customer1Auth)
        val agentJoin1 = agentRoom1.join()
        ensure(agentJoin1.scheduled)
        ensure(agentJoin1.actorId != agentAuth.actorId)
        ensure(agentJoin1.state.status == ConversationStatuses.WaitingForAgent)
        val joined1 = joined1ForCustomer.await().payload()
        ensure(joined1.conversationId == cid1)
        ensure(joined1.actorId == "agent-1")
        ensure(joined1.state.status == ConversationStatuses.Active)

        val greeting1ForCustomer = async { customer1.waitFor<ChatMessageNotify>().await() }
        val greet1 = agentRoom1.sendChat("How can I help?")
        ensure(greet1.message.messageSeq == 1L)
        val greeting1 = greeting1ForCustomer.await().payload()
        ensure(greeting1.conversationId == cid1)
        ensure(greeting1.message.messageSeq == 1L)

        val reply1ForAgent = async { agent.waitFor<ChatMessageNotify>().await() }
        val reply1 = customerRoom1.sendChat("Payment keeps failing.")
        ensure(reply1.message.messageSeq == 2L)
        val reply1Message = reply1ForAgent.await()
        ensure(reply1Message.actorId() == agentJoin1.actorId)
        val reply1Push = reply1Message.payload()
        ensure(reply1Push.conversationId == cid1)
        ensure(reply1Push.message.messageSeq == 2L)

        customer2.connect().await()
        val customer2Auth =
            customer2.request<AuthenticateRes>(AuthenticateReq("customer-2")).await()
        ensure(customer2Auth.actorId == "customer-2")
        val assigned2ForAgent = async { agent.waitFor<ConversationAssignedNotify>().await() }
        val opened2 =
            customer2.request<OpenConversationRes>(OpenConversationReq("cannot log in")).await()
        val cid2 = opened2.conversationId
        ensure(cid2 != cid1)
        ensure(opened2.state.status == ConversationStatuses.WaitingForAgent)
        ensure(assigned2ForAgent.await().payload().conversationId == cid2)

        val joined2ForCustomer = async { customer2.waitFor<ParticipantJoinedNotify>().await() }
        val agentRoom2 = ConversationClient(agent, cid2, agentAuth)
        val customerRoom2 = ConversationClient(customer2, cid2, customer2Auth)
        val agentJoin2 = agentRoom2.join()
        ensure(agentJoin2.scheduled)
        ensure(agentJoin2.actorId != agentJoin1.actorId)
        ensure(agentJoin2.state.status == ConversationStatuses.WaitingForAgent)
        ensure(joined2ForCustomer.await().payload().conversationId == cid2)

        val greeting2ForCustomer = async { customer2.waitFor<ChatMessageNotify>().await() }
        val greet2 = agentRoom2.sendChat("Let me check your account.")
        ensure(greet2.message.messageSeq == 1L)
        val greeting2 = greeting2ForCustomer.await().payload()
        ensure(greeting2.conversationId == cid2)
        ensure(greeting2.message.messageSeq == 1L)

        val typingForCustomer1 = async { customer1.waitFor<TypingChangedNotify>().await() }
        agentRoom1.sendTyping(true)
        val typing1 = typingForCustomer1.await().payload()
        ensure(typing1.conversationId == cid1)
        ensure(typing1.actorId == "agent-1")
        ensure(typing1.isTyping)

        customer1.close().await()
        reconnectingCustomer.connect().await()
        val reconnectedCustomerAuth =
            reconnectingCustomer.request<AuthenticateRes>(AuthenticateReq("customer-1")).await()
        ensure(reconnectedCustomerAuth.actorId == "customer-1")
        customerRoom1 = ConversationClient(reconnectingCustomer, cid1, reconnectedCustomerAuth)
        val customerRejoin1 = customerRoom1.join()
        ensure(!customerRejoin1.scheduled)
        ensure(customerRejoin1.state.subject == "checkout payment failed")
        ensure(customerRejoin1.state.status == ConversationStatuses.Active)
        ensure(customerRejoin1.state.lastMessageSeq == 2L)

        agent.close().await()
        reconnectingAgent.connect().await()
        val reconnectedAgentAuth =
            reconnectingAgent.request<AuthenticateRes>(AuthenticateReq("agent-1")).await()
        ensure(reconnectedAgentAuth.actorId == "agent-1")
        ensure(
            reconnectingAgent
                .request<SetAgentAvailableRes>(SetAgentAvailableReq(true))
                .await()
                .isAvailable
        )
        val reconnectedRoom1 = ConversationClient(reconnectingAgent, cid1, reconnectedAgentAuth)
        val reconnectedRoom2 = ConversationClient(reconnectingAgent, cid2, reconnectedAgentAuth)
        val agentRejoin1 = reconnectedRoom1.join()
        val agentRejoin2 = reconnectedRoom2.join()
        ensure(!agentRejoin1.scheduled)
        ensure(!agentRejoin2.scheduled)
        ensure(agentRejoin1.actorId == agentJoin1.actorId)
        ensure(agentRejoin2.actorId == agentJoin2.actorId)
        ensure(agentRejoin1.state.subject == "checkout payment failed")
        ensure(agentRejoin2.state.subject == "cannot log in")

        val idleTimeout =
            SampleTimings.IdleTimeout.plus(SampleTimings.CloseGraceTimeout)
                .plus(SampleTimings.RequestTimeout)
        val idle1ForCustomer =
            async(start = CoroutineStart.UNDISPATCHED) {
                reconnectingCustomer.waitFor<ConversationIdleNotify>().timeout(idleTimeout).await()
            }
        val idle1ForAgent =
            async(start = CoroutineStart.UNDISPATCHED) {
                reconnectingAgent
                    .waitFor<ConversationIdleNotify>()
                    .where { it.payload().conversationId == cid1 }
                    .timeout(idleTimeout)
                    .await()
            }
        val closed1ForCustomer =
            async(start = CoroutineStart.UNDISPATCHED) {
                reconnectingCustomer
                    .waitFor<ConversationClosedNotify>()
                    .timeout(idleTimeout)
                    .await()
            }
        val closed1ForAgent =
            async(start = CoroutineStart.UNDISPATCHED) {
                reconnectingAgent
                    .waitFor<ConversationClosedNotify>()
                    .where { it.payload().conversationId == cid1 }
                    .timeout(idleTimeout)
                    .await()
            }
        val closed2ForAgent =
            async(start = CoroutineStart.UNDISPATCHED) {
                reconnectingAgent
                    .waitFor<ConversationClosedNotify>()
                    .where { it.payload().conversationId == cid2 }
                    .timeout(idleTimeout)
                    .await()
            }
        val closed2 = customerRoom2.close("resolved")
        ensure(closed2.state.status == ConversationStatuses.Closed)
        val closed2Message = closed2ForAgent.await()
        ensure(closed2Message.actorId() == agentRejoin2.actorId)
        val closed2Agent = closed2Message.payload()
        ensure(closed2Agent.conversationId == cid2)
        ensure(closed2Agent.state.status == ConversationStatuses.Closed)

        // --8<-- [start:doc-e2e-failure]
        ZLinkKotlinStreamAssert.expectFailure { customerRoom2.close("again") }
        // --8<-- [end:doc-e2e-failure]

        ensure(
            idle1ForCustomer.await().payload().state.status == ConversationStatuses.WaitingForClose
        )
        val idle1Message = idle1ForAgent.await()
        ensure(idle1Message.actorId() == agentRejoin1.actorId)
        val idle1Agent = idle1Message.payload()
        ensure(idle1Agent.conversationId == cid1)
        ensure(idle1Agent.state.status == ConversationStatuses.WaitingForClose)
        ensure(closed1ForCustomer.await().payload().state.status == ConversationStatuses.Closed)
        val closed1Message = closed1ForAgent.await()
        ensure(closed1Message.actorId() == agentRejoin1.actorId)
        val closed1Agent = closed1Message.payload()
        ensure(closed1Agent.conversationId == cid1)
        ensure(closed1Agent.state.status == ConversationStatuses.Closed)

        ZLinkKotlinStreamAssert.expectFailure { customerRoom1.sendChat("are you there?") }
        val closedTypingForAgent =
            async(start = CoroutineStart.UNDISPATCHED) {
                reconnectingAgent
                    .expectNone<TypingChangedNotify>(TypingChangedNotify::class.java.simpleName)
                    .within(Duration.ofMillis(500))
                    .await()
            }
        customerRoom1.sendTyping(true)
        closedTypingForAgent.await()
        println("supportchat-closed-typing-ignore=verified")

        ensure(
            !reconnectingAgent
                .request<SetAgentAvailableRes>(SetAgentAvailableReq(false))
                .await()
                .isAvailable
        )
        waitingCustomer.connect().await()
        ensure(
            waitingCustomer
                .request<AuthenticateRes>(AuthenticateReq("customer-3"))
                .await()
                .actorId == "customer-3"
        )
        ZLinkKotlinStreamAssert.expectFailure {
            waitingCustomer.request<SetAgentAvailableRes>(SetAgentAvailableReq(true)).await()
        }
        val noClosedNotification =
            async(start = CoroutineStart.UNDISPATCHED) {
                waitingCustomer
                    .expectNone<ConversationClosedNotify>(
                        ConversationClosedNotify::class.java.simpleName
                    )
                    .within(Duration.ofMillis(500))
                    .await()
            }
        val noAgentOpen =
            waitingCustomer
                .request<OpenConversationRes>(OpenConversationReq("agent unavailable"))
                .await()
        ensure(noAgentOpen.state.status == ConversationStatuses.WaitingForAgent)
        ensure(noAgentOpen.state.subject == "agent unavailable")
        noClosedNotification.await()
    }

    private class ConversationClient(
        private val connector: ZLinkKotlinStreamConnector,
        private val conversationId: String,
        private val identity: AuthenticateRes,
    ) {
        private var roomActor: ZLinkKotlinStreamActor? = null
        private val isAgent = identity.role == SupportChatRoles.Agent

        suspend fun join(): JoinConversationRes {
            val joined =
                connector
                    .request<JoinConversationRes>(
                        JoinConversationReq(
                            conversationId,
                            identity.actorId,
                            identity.role,
                            identity.displayName,
                        )
                    )
                    .await()
            if (isAgent) {
                roomActor =
                    connector.actor(joined.actorId)
                        ?: error("Conversation actor is not bound. actor=${joined.actorId}")
            }
            return joined
        }

        suspend fun sendChat(text: String): SendChatMessageRes =
            if (isAgent) {
                requireNotNull(roomActor)
                    .request<SendChatMessageRes>(SendChatMessageReq(text))
                    .await()
            } else {
                connector.request<SendChatMessageRes>(SendChatMessageReq(text)).await()
            }

        suspend fun sendTyping(isTyping: Boolean) {
            if (isAgent) {
                requireNotNull(roomActor).send(SetTypingMsg(isTyping)).await()
            } else {
                connector.send(SetTypingMsg(isTyping)).await()
            }
        }

        suspend fun close(reason: String?): CloseConversationRes =
            if (isAgent) {
                requireNotNull(roomActor)
                    .request<CloseConversationRes>(CloseConversationReq(reason))
                    .await()
            } else {
                connector.request<CloseConversationRes>(CloseConversationReq(reason)).await()
            }
    }
}

private fun ensure(condition: Boolean) {
    if (!condition) {
        throw IllegalStateException("Ensure failed")
    }
}
