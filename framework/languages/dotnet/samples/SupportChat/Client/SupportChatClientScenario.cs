using SupportChat.Client.Configuration;
using SupportChat.Shared.Contracts;
using Systems.Zlink.Stream.Connector.Contracts;

namespace SupportChat.Client;

internal sealed class SupportChatClientScenario
{
    private const string Cid = SampleNames.ConversationIdMetadataKey;

    // End-to-end client story for one agent : many customers (§16, §17):
    // 1. One agent registers availability.
    // 2. Two customers open conversations; the same agent is assigned to both and joins each.
    // 3. Each room keeps its own MessageSeq; typing is one-way.
    // 4. The customer and agent reconnect and re-join their rooms.
    // 5. One room auto-closes on idle; the other is closed explicitly; then messages are rejected.
    // 6. With no available agent, a new customer stays WaitingForAgent.
    public async ValueTask RunAsync(
        IZlinkStreamConnector agent,
        IZlinkStreamConnector customer1,
        IZlinkStreamConnector customer2,
        IZlinkStreamConnector reconnectingAgent,
        IZlinkStreamConnector reconnectingCustomer,
        IZlinkStreamConnector waitingCustomer,
        CancellationToken cancellationToken = default)
    {
        await agent.Connect.Async(cancellationToken);
        await ZlinkStreamAssert.ExpectFailureAsync(
            async ct => _ = await agent.Request(new OpenConversationReq("unauthenticated"))
                .Async<OpenConversationRes>(ct),
            nameof(ZlinkStreamErrorCode.RemoteError));
        await ZlinkStreamAssert.ExpectFailureAsync(
            async ct => _ = await agent.Request(new SendChatMessageReq("unauthenticated"))
                .Metadata(Cid, "missing-conversation")
                .Async<SendChatMessageRes>(ct),
            nameof(ZlinkStreamErrorCode.RemoteError));

        var agentAuth = await agent.Request(new AuthenticateReq("agent-1")).Async<AuthenticateRes>(cancellationToken);
        ZlinkStreamAssert.Ensure(agentAuth.ActorId == "agent-1", "Assertion failed: agentAuth.ActorId == \"agent-1\"");
        ZlinkStreamAssert.Ensure(agentAuth.Role == SupportChatRoles.Agent, "Assertion failed: agentAuth.Role == SupportChatRoles.Agent");
        await ZlinkStreamAssert.ExpectFailureAsync(
            async ct => _ = await agent.Request(new OpenConversationReq("agent cannot open"))
                .Async<OpenConversationRes>(ct),
            nameof(ZlinkStreamErrorCode.RemoteError));
        ZlinkStreamAssert.Ensure((await agent.Request(new SetAgentAvailableReq(true))
            .Async<SetAgentAvailableRes>(cancellationToken)).IsAvailable, "Assertion failed: (await agent.Request(new SetAgentAvailableReq(true)) .Async<SetAgentAvailableRes>(cancellationToken)).IsAvailable");

        // Customer 1 opens a conversation; the agent's roster is notified, then the
        // agent joins the conversation which turns it Active.
        await customer1.Connect.Async(cancellationToken);
        ZlinkStreamAssert.Ensure((await customer1.Request(new AuthenticateReq("customer-1"))
            .Async<AuthenticateRes>(cancellationToken)).ActorId == "customer-1", "Assertion failed: (await customer1.Request(new AuthenticateReq(\"customer-1\")) .Async<AuthenticateRes>(cancellationToken)).ActorId == \"customer-1\"");
        var assigned1ForAgent = agent.WaitFor<ConversationAssignedNotify>().Async(cancellationToken);
        var opened1 = await customer1.Request(new OpenConversationReq("checkout payment failed"))
            .Async<OpenConversationRes>(cancellationToken);
        var cid1 = opened1.ConversationId;
        ZlinkStreamAssert.Ensure(opened1.State.Status == ConversationStatuses.WaitingForAgent, "Assertion failed: opened1.State.Status == ConversationStatuses.WaitingForAgent");
        ZlinkStreamAssert.Ensure((await assigned1ForAgent).Payload.ConversationId == cid1, "Assertion failed: (await assigned1ForAgent).Payload.ConversationId == cid1");

        var joined1ForCustomer = customer1.WaitFor<ParticipantJoinedNotify>().Async(cancellationToken);
        var joined1ForAgent = agent.WaitFor<ParticipantJoinedNotify>()
            .Where(message => message.Payload.ConversationId == cid1)
            .Async(cancellationToken);
        var agentRoom1 = Conversation(agent, cid1);
        var customerRoom1 = Conversation(customer1, cid1);
        var agentJoin1 = await agentRoom1.JoinAsync(cancellationToken);
        ZlinkStreamAssert.Ensure(agentJoin1.Scheduled, "Assertion failed: agentJoin1.Scheduled");
        var joined1 = await joined1ForCustomer;
        var joined1Agent = await joined1ForAgent;
        ZlinkStreamAssert.Ensure(joined1.Payload.ConversationId == cid1, "Assertion failed: joined1.Payload.ConversationId == cid1");
        ZlinkStreamAssert.Ensure(joined1.Payload.ActorId == "agent-1", "Assertion failed: joined1.Payload.ActorId == \"agent-1\"");
        ZlinkStreamAssert.Ensure(joined1.Payload.State.Status == ConversationStatuses.Active, "Assertion failed: joined1.Payload.State.Status == ConversationStatuses.Active");
        ZlinkStreamAssert.Ensure(joined1Agent.Payload.State.AgentActorId == "agent-1", "Assertion failed: joined1Agent.Payload.State.AgentActorId == \"agent-1\"");
        ZlinkStreamAssert.Ensure(joined1Agent.Payload.State.Subject == "checkout payment failed", "Assertion failed: joined1Agent.Payload.State.Subject == \"checkout payment failed\"");

        // Agent greets, customer replies; sequence is assigned per conversation.
        var greeting1ForCustomer = customer1.WaitFor<ChatMessageNotify>().Async(cancellationToken);
        var greet1 = await agentRoom1.SendChatAsync("How can I help?", cancellationToken);
        ZlinkStreamAssert.Ensure(greet1.Message.MessageSeq == 1UL, "Assertion failed: greet1.Message.MessageSeq == 1UL");
        var greeting1 = await greeting1ForCustomer;
        ZlinkStreamAssert.Ensure(greeting1.Payload.ConversationId == cid1, "Assertion failed: greeting1.Payload.ConversationId == cid1");
        ZlinkStreamAssert.Ensure(greeting1.Payload.Message.MessageSeq == 1UL, "Assertion failed: greeting1.Payload.Message.MessageSeq == 1UL");

        var reply1ForAgent = agent.WaitFor<ChatMessageNotify>().Async(cancellationToken);
        var reply1 = await customerRoom1.SendChatAsync("Payment keeps failing.", cancellationToken);
        ZlinkStreamAssert.Ensure(reply1.Message.MessageSeq == 2UL, "Assertion failed: reply1.Message.MessageSeq == 2UL");
        var reply1Push = await reply1ForAgent;
        ZlinkStreamAssert.Ensure(reply1Push.Payload.ConversationId == cid1, "Assertion failed: reply1Push.Payload.ConversationId == cid1");
        ZlinkStreamAssert.Ensure(reply1Push.Payload.Message.MessageSeq == 2UL, "Assertion failed: reply1Push.Payload.Message.MessageSeq == 2UL");

        // Customer 2 opens a second conversation handled by the SAME agent.
        await customer2.Connect.Async(cancellationToken);
        ZlinkStreamAssert.Ensure((await customer2.Request(new AuthenticateReq("customer-2"))
            .Async<AuthenticateRes>(cancellationToken)).ActorId == "customer-2", "Assertion failed: (await customer2.Request(new AuthenticateReq(\"customer-2\")) .Async<AuthenticateRes>(cancellationToken)).ActorId == \"customer-2\"");
        var assigned2ForAgent = agent.WaitFor<ConversationAssignedNotify>().Async(cancellationToken);
        var opened2 = await customer2.Request(new OpenConversationReq("cannot log in"))
            .Async<OpenConversationRes>(cancellationToken);
        var cid2 = opened2.ConversationId;
        ZlinkStreamAssert.Ensure(cid2 != cid1, "Assertion failed: cid2 != cid1");
        ZlinkStreamAssert.Ensure(opened2.State.Status == ConversationStatuses.WaitingForAgent, "Assertion failed: opened2.State.Status == ConversationStatuses.WaitingForAgent");
        ZlinkStreamAssert.Ensure((await assigned2ForAgent).Payload.ConversationId == cid2, "Assertion failed: (await assigned2ForAgent).Payload.ConversationId == cid2");

        var joined2ForCustomer = customer2.WaitFor<ParticipantJoinedNotify>().Async(cancellationToken);
        var joined2ForAgent = agent.WaitFor<ParticipantJoinedNotify>()
            .Where(message => message.Payload.ConversationId == cid2)
            .Async(cancellationToken);
        var agentRoom2 = Conversation(agent, cid2);
        var customerRoom2 = Conversation(customer2, cid2);
        var agentJoin2 = await agentRoom2.JoinAsync(cancellationToken);
        ZlinkStreamAssert.Ensure(agentJoin2.Scheduled, "Assertion failed: agentJoin2.Scheduled");
        ZlinkStreamAssert.Ensure((await joined2ForCustomer).Payload.ConversationId == cid2, "Assertion failed: (await joined2ForCustomer).Payload.ConversationId == cid2");
        ZlinkStreamAssert.Ensure((await joined2ForAgent).Payload.State.Subject == "cannot log in", "Assertion failed: (await joined2ForAgent).Payload.State.Subject == \"cannot log in\"");

        // The rooms are independent: cid2 starts its own MessageSeq at 1.
        var greeting2ForCustomer = customer2.WaitFor<ChatMessageNotify>().Async(cancellationToken);
        var greet2 = await agentRoom2.SendChatAsync("Let me check your account.", cancellationToken);
        ZlinkStreamAssert.Ensure(greet2.Message.MessageSeq == 1UL, "Assertion failed: greet2.Message.MessageSeq == 1UL");
        var greeting2 = await greeting2ForCustomer;
        ZlinkStreamAssert.Ensure(greeting2.Payload.ConversationId == cid2, "Assertion failed: greeting2.Payload.ConversationId == cid2");
        ZlinkStreamAssert.Ensure(greeting2.Payload.Message.MessageSeq == 1UL, "Assertion failed: greeting2.Payload.Message.MessageSeq == 1UL");

        // Typing is a one-way send; only the other participant is notified.
        var typingForCustomer1 = customer1.WaitFor<TypingChangedNotify>().Async(cancellationToken);
        await agentRoom1.SendTypingAsync(true, cancellationToken);
        var typing1 = await typingForCustomer1;
        ZlinkStreamAssert.Ensure(typing1.Payload.ConversationId == cid1, "Assertion failed: typing1.Payload.ConversationId == cid1");
        ZlinkStreamAssert.Ensure(typing1.Payload.ActorId == "agent-1", "Assertion failed: typing1.Payload.ActorId == \"agent-1\"");
        ZlinkStreamAssert.Ensure(typing1.Payload.IsTyping, "Assertion failed: typing1.Payload.IsTyping");

        // Reconnect customer 1 with the same token and re-read the current room state.
        await customer1.Close.Async(cancellationToken);
        await reconnectingCustomer.Connect.Async(cancellationToken);
        ZlinkStreamAssert.Ensure((await reconnectingCustomer.Request(new AuthenticateReq("customer-1"))
            .Async<AuthenticateRes>(cancellationToken)).ActorId == "customer-1", "Assertion failed: (await reconnectingCustomer.Request(new AuthenticateReq(\"customer-1\")) .Async<AuthenticateRes>(cancellationToken)).ActorId == \"customer-1\"");
        customerRoom1 = Conversation(reconnectingCustomer, cid1);
        var customerRejoin1 = await customerRoom1.JoinAsync(cancellationToken);
        ZlinkStreamAssert.Ensure(!customerRejoin1.Scheduled, "Assertion failed: !customerRejoin1.Scheduled");
        ZlinkStreamAssert.Ensure(customerRejoin1.State.Subject == "checkout payment failed", "Assertion failed: customerRejoin1.State.Subject == \"checkout payment failed\"");
        ZlinkStreamAssert.Ensure(customerRejoin1.State.Status == ConversationStatuses.Active, "Assertion failed: customerRejoin1.State.Status == ConversationStatuses.Active");
        ZlinkStreamAssert.Ensure(customerRejoin1.State.LastMessageSeq == 2UL, "Assertion failed: customerRejoin1.State.LastMessageSeq == 2UL");

        // Reconnect the agent on a fresh session. Close the old connection first (as a
        // real reconnect does), re-declare availability so the roster's push route
        // follows the new session, then re-join both rooms.
        await agent.Close.Async(cancellationToken);
        await reconnectingAgent.Connect.Async(cancellationToken);
        ZlinkStreamAssert.Ensure((await reconnectingAgent.Request(new AuthenticateReq("agent-1"))
            .Async<AuthenticateRes>(cancellationToken)).ActorId == "agent-1", "Assertion failed: (await reconnectingAgent.Request(new AuthenticateReq(\"agent-1\")) .Async<AuthenticateRes>(cancellationToken)).ActorId == \"agent-1\"");
        ZlinkStreamAssert.Ensure((await reconnectingAgent.Request(new SetAgentAvailableReq(true))
            .Async<SetAgentAvailableRes>(cancellationToken)).IsAvailable, "Assertion failed: (await reconnectingAgent.Request(new SetAgentAvailableReq(true)) .Async<SetAgentAvailableRes>(cancellationToken)).IsAvailable");
        var reconnectedRoom1 = Conversation(reconnectingAgent, cid1);
        var reconnectedRoom2 = Conversation(reconnectingAgent, cid2);
        var rejoinedRoom1 = await reconnectedRoom1.JoinAsync(cancellationToken);
        var rejoinedRoom2 = await reconnectedRoom2.JoinAsync(cancellationToken);
        ZlinkStreamAssert.Ensure(!rejoinedRoom1.Scheduled, "Assertion failed: !rejoinedRoom1.Scheduled");
        ZlinkStreamAssert.Ensure(!rejoinedRoom2.Scheduled, "Assertion failed: !rejoinedRoom2.Scheduled");
        ZlinkStreamAssert.Ensure(rejoinedRoom1.State.Subject == "checkout payment failed", "Assertion failed: rejoinedRoom1.State.Subject == \"checkout payment failed\"");
        ZlinkStreamAssert.Ensure(rejoinedRoom2.State.Subject == "cannot log in", "Assertion failed: rejoinedRoom2.State.Subject == \"cannot log in\"");

        // Arm cid1 idle + auto-close waiters (both sides) before cid1 can close.
        var idleTimeout = SampleNames.IdleTimeout + SampleNames.CloseGraceTimeout + SampleNames.RequestTimeout;
        var idle1ForCustomer = reconnectingCustomer.WaitFor<ConversationIdleNotify>().Timeout(idleTimeout).Async(cancellationToken);
        var idle1ForAgent = reconnectingAgent.WaitFor<ConversationIdleNotify>()
            .Where(m => m.Payload.ConversationId == cid1).Timeout(idleTimeout).Async(cancellationToken);
        var closed1ForCustomer = reconnectingCustomer.WaitFor<ConversationClosedNotify>().Timeout(idleTimeout).Async(cancellationToken);
        var closed1ForAgent = reconnectingAgent.WaitFor<ConversationClosedNotify>()
            .Where(m => m.Payload.ConversationId == cid1).Timeout(idleTimeout).Async(cancellationToken);

        // Explicitly close cid2 from the customer; only the agent is notified.
        var closed2ForAgent = reconnectingAgent.WaitFor<ConversationClosedNotify>()
            .Where(m => m.Payload.ConversationId == cid2).Timeout(idleTimeout).Async(cancellationToken);
        var closed2 = await customerRoom2.CloseAsync("resolved", cancellationToken);
        ZlinkStreamAssert.Ensure(closed2.State.Status == ConversationStatuses.Closed, "Assertion failed: closed2.State.Status == ConversationStatuses.Closed");
        var closed2Agent = await closed2ForAgent;
        ZlinkStreamAssert.Ensure(closed2Agent.Payload.ConversationId == cid2, "Assertion failed: closed2Agent.Payload.ConversationId == cid2");
        ZlinkStreamAssert.Ensure(closed2Agent.Payload.State.Status == ConversationStatuses.Closed, "Assertion failed: closed2Agent.Payload.State.Status == ConversationStatuses.Closed");

        // Closing an already-closed conversation returns an error response.
        await ZlinkStreamAssert.ExpectFailureAsync(
            async ct => _ = await customerRoom2.CloseAsync("again", ct),
            nameof(ZlinkStreamErrorCode.RemoteError));

        // cid1 idles first (both sides notified WaitingForClose), then auto-closes.
        ZlinkStreamAssert.Ensure((await idle1ForCustomer).Payload.State.Status == ConversationStatuses.WaitingForClose, "Assertion failed: (await idle1ForCustomer).Payload.State.Status == ConversationStatuses.WaitingForClose");
        var idle1Agent = await idle1ForAgent;
        ZlinkStreamAssert.Ensure(idle1Agent.Payload.ConversationId == cid1, "Assertion failed: idle1Agent.Payload.ConversationId == cid1");
        ZlinkStreamAssert.Ensure(idle1Agent.Payload.State.Status == ConversationStatuses.WaitingForClose, "Assertion failed: idle1Agent.Payload.State.Status == ConversationStatuses.WaitingForClose");
        ZlinkStreamAssert.Ensure((await closed1ForCustomer).Payload.State.Status == ConversationStatuses.Closed, "Assertion failed: (await closed1ForCustomer).Payload.State.Status == ConversationStatuses.Closed");
        var closed1Agent = await closed1ForAgent;
        ZlinkStreamAssert.Ensure(closed1Agent.Payload.ConversationId == cid1, "Assertion failed: closed1Agent.Payload.ConversationId == cid1");
        ZlinkStreamAssert.Ensure(closed1Agent.Payload.State.Status == ConversationStatuses.Closed, "Assertion failed: closed1Agent.Payload.State.Status == ConversationStatuses.Closed");

        // A closed conversation rejects further messages.
        await ZlinkStreamAssert.ExpectFailureAsync(
            async ct => _ = await customerRoom1.SendChatAsync("are you there?", ct),
            nameof(ZlinkStreamErrorCode.RemoteError));
        await customerRoom1.SendTypingAsync(true, cancellationToken);
        await reconnectingAgent.ExpectNone<TypingChangedNotify>()
            .Within(TimeSpan.FromMilliseconds(500))
            .Async(cancellationToken);
        Console.WriteLine("supportchat-closed-typing-ignore=verified");

        // With the agent unavailable and no capacity elsewhere, a new customer waits.
        ZlinkStreamAssert.Ensure(!(await reconnectingAgent.Request(new SetAgentAvailableReq(false))
            .Async<SetAgentAvailableRes>(cancellationToken)).IsAvailable, "Assertion failed: !(await reconnectingAgent.Request(new SetAgentAvailableReq(false)) .Async<SetAgentAvailableRes>(cancellationToken)).IsAvailable");
        await waitingCustomer.Connect.Async(cancellationToken);
        ZlinkStreamAssert.Ensure((await waitingCustomer.Request(new AuthenticateReq("customer-3"))
            .Async<AuthenticateRes>(cancellationToken)).ActorId == "customer-3", "Assertion failed: (await waitingCustomer.Request(new AuthenticateReq(\"customer-3\")) .Async<AuthenticateRes>(cancellationToken)).ActorId == \"customer-3\"");

        // A customer actor cannot register agent availability.
        await ZlinkStreamAssert.ExpectFailureAsync(
            async ct => _ = await waitingCustomer.Request(new SetAgentAvailableReq(true))
                .Async<SetAgentAvailableRes>(ct),
            nameof(ZlinkStreamErrorCode.RemoteError));

        var noAgentOpen = await waitingCustomer.Request(new OpenConversationReq("agent unavailable"))
            .Async<OpenConversationRes>(cancellationToken);
        ZlinkStreamAssert.Ensure(noAgentOpen.State.Status == ConversationStatuses.WaitingForAgent, "Assertion failed: noAgentOpen.State.Status == ConversationStatuses.WaitingForAgent");
        ZlinkStreamAssert.Ensure(noAgentOpen.State.Subject == "agent unavailable", "Assertion failed: noAgentOpen.State.Subject == \"agent unavailable\"");
        // customer-3 now belongs to its own room. Supplying cid2 must not silently send this
        // message to customer-3's current room.
        await ZlinkStreamAssert.ExpectFailureAsync(
            async ct => _ = await waitingCustomer.Request(new SendChatMessageReq("not a participant"))
                .Metadata(Cid, cid2)
                .Async<SendChatMessageRes>(ct),
            nameof(ZlinkStreamErrorCode.RemoteError));
        await waitingCustomer.ExpectNone<ConversationClosedNotify>()
            .Within(TimeSpan.FromMilliseconds(500))
            .Async(cancellationToken);
    }

    private static ConversationClient Conversation(IZlinkStreamConnector connector, string conversationId) =>
        new(connector, conversationId);

    private readonly struct ConversationClient(IZlinkStreamConnector connector, string conversationId)
    {
        // ConversationId rides in stream metadata, not in the body. Keeping that detail
        // here lets the scenario read as conversation work instead of route wiring.
        public ValueTask<JoinConversationRes> JoinAsync(CancellationToken cancellationToken)
        {
            return JoinCoreAsync(cancellationToken);
        }

        private async ValueTask<JoinConversationRes> JoinCoreAsync(
            CancellationToken cancellationToken)
        {
            var completion = connector.WaitFor<JoinConversationRes>()
                .Async(cancellationToken);
            await connector.Send(new JoinConversationReq())
                .Metadata(Cid, conversationId)
                .Async(cancellationToken);
            return (await completion).Payload;
        }

        public ValueTask<SendChatMessageRes> SendChatAsync(string text, CancellationToken cancellationToken)
        {
            return connector.Request(new SendChatMessageReq(text)).Metadata(Cid, conversationId)
                .Async<SendChatMessageRes>(cancellationToken);
        }

        public ValueTask SendTypingAsync(bool isTyping, CancellationToken cancellationToken)
        {
            return connector.Send(new SetTypingMsg(isTyping)).Metadata(Cid, conversationId).Async(cancellationToken);
        }

        public ValueTask<CloseConversationRes> CloseAsync(string? reason, CancellationToken cancellationToken)
        {
            return connector.Request(new CloseConversationReq(reason)).Metadata(Cid, conversationId)
                .Async<CloseConversationRes>(cancellationToken);
        }
    }

}
