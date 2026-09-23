import {
  ConversationStatuses,
  PacketNames,
  SupportChatRoles,
  authenticate,
  closeConversation,
  joinConversation,
  openConversation,
  sendChatMessage,
  setAgentAvailable,
  setTyping
} from '../Shared/Contracts/messages';
import type {
  AuthenticateRes,
  ChatMessageNotify,
  CloseConversationRes,
  ConversationAssignedNotify,
  ConversationClosedNotify,
  ConversationIdleNotify,
  JoinConversationRes,
  OpenConversationRes,
  ParticipantJoinedNotify,
  SendChatMessageRes,
  SetAgentAvailableRes,
  TypingChangedNotify
} from '../Shared/Contracts/messages';
import { zlinkStreamAssert, type ZlinkStreamConnector } from '@zlink-systems/stream-connector';

class SupportChatClientScenario {
  async run(
    agent: ZlinkStreamConnector,
    customer1: ZlinkStreamConnector,
    customer2: ZlinkStreamConnector,
    reconnectedAgent: ZlinkStreamConnector,
    reconnectedCustomer: ZlinkStreamConnector,
    customer3: ZlinkStreamConnector,
    customer4: ZlinkStreamConnector,
    customer5: ZlinkStreamConnector,
    customer6: ZlinkStreamConnector,
    signal?: AbortSignal
  ): Promise<void> {
    await agent.connect(signal);
    const agentAuth = await request<AuthenticateRes>(
      agent,
      PacketNames.authenticateReq,
      authenticate('agent-1'),
      undefined,
      signal
    );
    zlinkStreamAssert.ensure(
      agentAuth.role === SupportChatRoles.Agent,
      'Sample scenario assertion failed.'
    );
    const available = await request<SetAgentAvailableRes>(
      agent,
      PacketNames.setAgentAvailableReq,
      setAgentAvailable(true),
      undefined,
      signal
    );
    zlinkStreamAssert.ensure(available.isAvailable, 'Sample scenario assertion failed.');
    // --8<-- [start:doc-e2e-failure]
    await zlinkStreamAssert.expectFailure(async () => {
      await request(
        agent,
        PacketNames.openConversationReq,
        openConversation('agent must not open'),
        undefined,
        signal
      );
    });
    // --8<-- [end:doc-e2e-failure]

    await customer1.connect(signal);
    const customer1Auth = await request<AuthenticateRes>(
      customer1,
      PacketNames.authenticateReq,
      authenticate('customer-1'),
      undefined,
      signal
    );
    zlinkStreamAssert.ensure(
      customer1Auth.role === SupportChatRoles.Customer,
      'Sample scenario assertion failed.'
    );
    const assigned1Task = wait<ConversationAssignedNotify>(
      agent,
      PacketNames.conversationAssignedNotify,
      signal
    );
    const opened1 = await request<OpenConversationRes>(
      customer1,
      PacketNames.openConversationReq,
      openConversation('checkout payment failed'),
      undefined,
      signal
    );
    const cid1 = opened1.conversationId;
    zlinkStreamAssert.ensure(
      opened1.state.status === ConversationStatuses.WaitingForAgent,
      'Sample scenario assertion failed.'
    );
    zlinkStreamAssert.ensure(
      opened1.state.subject === 'checkout payment failed',
      'Sample scenario assertion failed.'
    );
    const assigned1 = await assigned1Task;
    zlinkStreamAssert.ensure(
      assigned1.payload.conversationId === cid1,
      'Sample scenario assertion failed.'
    );

    const joinedCustomer1 = wait<ParticipantJoinedNotify>(
      customer1,
      PacketNames.participantJoinedNotify,
      signal
    );
    const joinedAgent1 = wait<ParticipantJoinedNotify>(
      agent,
      PacketNames.participantJoinedNotify,
      signal
    );
    const agentJoin1 = await request<JoinConversationRes>(
      agent,
      PacketNames.joinConversationReq,
      joinConversation(cid1),
      undefined,
      signal
    );
    zlinkStreamAssert.ensure(agentJoin1.scheduled, 'Sample scenario assertion failed.');
    const agentRoom1 = requireActor(agent, agentJoin1.actorId);
    zlinkStreamAssert.ensure(
      agentJoin1.state.status === ConversationStatuses.WaitingForAgent,
      'Sample scenario assertion failed.'
    );
    const [customer1Joined, agent1Joined] = await Promise.all([joinedCustomer1, joinedAgent1]);
    zlinkStreamAssert.ensure(
      customer1Joined.payload.actorId === 'agent-1',
      'Sample scenario assertion failed.'
    );
    zlinkStreamAssert.ensure(
      customer1Joined.payload.conversationId === cid1,
      'Sample scenario assertion failed.'
    );
    zlinkStreamAssert.ensure(
      customer1Joined.payload.role === SupportChatRoles.Agent,
      'Sample scenario assertion failed.'
    );
    zlinkStreamAssert.ensure(
      customer1Joined.payload.state.status === ConversationStatuses.Active,
      'Sample scenario assertion failed.'
    );
    zlinkStreamAssert.ensure(
      agent1Joined.payload.conversationId === cid1,
      'Sample scenario assertion failed.'
    );
    zlinkStreamAssert.ensure(
      agent1Joined.actorId === agentRoom1.actorId,
      'Sample scenario assertion failed.'
    );
    zlinkStreamAssert.ensure(
      agent1Joined.payload.state.status === ConversationStatuses.Active,
      'Sample scenario assertion failed.'
    );

    const greeting1Task = wait<ChatMessageNotify>(customer1, PacketNames.chatMessageNotify, signal);
    const greeting1 = await request<SendChatMessageRes>(
      agent,
      PacketNames.sendChatMessageReq,
      sendChatMessage('How can I help?'),
      agentRoom1.actorId,
      signal
    );
    zlinkStreamAssert.ensure(
      greeting1.message.messageSeq === 1,
      'Sample scenario assertion failed.'
    );
    zlinkStreamAssert.ensure(
      greeting1.message.conversationId === cid1,
      'Sample scenario assertion failed.'
    );
    zlinkStreamAssert.ensure(
      greeting1.message.senderActorId === 'agent-1',
      'Sample scenario assertion failed.'
    );
    zlinkStreamAssert.ensure(
      greeting1.message.text === 'How can I help?',
      'Sample scenario assertion failed.'
    );
    zlinkStreamAssert.ensure(
      greeting1.state.status === ConversationStatuses.Active,
      'Sample scenario assertion failed.'
    );
    const greeting1Push = await greeting1Task;
    zlinkStreamAssert.ensure(
      greeting1Push.payload.message.messageSeq === 1,
      'Sample scenario assertion failed.'
    );
    zlinkStreamAssert.ensure(
      greeting1Push.payload.conversationId === cid1,
      'Sample scenario assertion failed.'
    );
    zlinkStreamAssert.ensure(
      greeting1Push.payload.message.conversationId === cid1,
      'Sample scenario assertion failed.'
    );
    zlinkStreamAssert.ensure(
      greeting1Push.payload.message.senderActorId === 'agent-1',
      'Sample scenario assertion failed.'
    );
    zlinkStreamAssert.ensure(
      greeting1Push.payload.message.text === 'How can I help?',
      'Sample scenario assertion failed.'
    );
    zlinkStreamAssert.ensure(
      greeting1Push.payload.state.status === ConversationStatuses.Active,
      'Sample scenario assertion failed.'
    );
    const reply1Task = wait<ChatMessageNotify>(agent, PacketNames.chatMessageNotify, signal);
    const reply1 = await request<SendChatMessageRes>(
      customer1,
      PacketNames.sendChatMessageReq,
      sendChatMessage('Payment keeps failing.'),
      undefined,
      signal
    );
    zlinkStreamAssert.ensure(reply1.message.messageSeq === 2, 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(
      reply1.message.conversationId === cid1,
      'Sample scenario assertion failed.'
    );
    zlinkStreamAssert.ensure(
      reply1.message.senderActorId === 'customer-1',
      'Sample scenario assertion failed.'
    );
    zlinkStreamAssert.ensure(
      reply1.message.text === 'Payment keeps failing.',
      'Sample scenario assertion failed.'
    );
    const reply1Push = await reply1Task;
    zlinkStreamAssert.ensure(
      reply1Push.actorId === agentRoom1.actorId,
      'Sample scenario assertion failed.'
    );
    zlinkStreamAssert.ensure(
      reply1Push.payload.message.messageSeq === 2,
      'Sample scenario assertion failed.'
    );
    zlinkStreamAssert.ensure(
      reply1Push.payload.conversationId === cid1,
      'Sample scenario assertion failed.'
    );
    zlinkStreamAssert.ensure(
      reply1Push.payload.message.conversationId === cid1,
      'Sample scenario assertion failed.'
    );
    zlinkStreamAssert.ensure(
      reply1Push.payload.message.senderActorId === 'customer-1',
      'Sample scenario assertion failed.'
    );
    zlinkStreamAssert.ensure(
      reply1Push.payload.message.text === 'Payment keeps failing.',
      'Sample scenario assertion failed.'
    );

    await customer2.connect(signal);
    await request<AuthenticateRes>(
      customer2,
      PacketNames.authenticateReq,
      authenticate('customer-2'),
      undefined,
      signal
    );
    const assigned2Task = wait<ConversationAssignedNotify>(
      agent,
      PacketNames.conversationAssignedNotify,
      signal
    );
    const opened2 = await request<OpenConversationRes>(
      customer2,
      PacketNames.openConversationReq,
      openConversation('cannot log in'),
      undefined,
      signal
    );
    const cid2 = opened2.conversationId;
    zlinkStreamAssert.ensure(cid2 !== cid1, 'Sample scenario assertion failed.');
    const assigned2 = await assigned2Task;
    zlinkStreamAssert.ensure(
      assigned2.payload.conversationId === cid2,
      'Sample scenario assertion failed.'
    );
    const joinedCustomer2 = wait<ParticipantJoinedNotify>(
      customer2,
      PacketNames.participantJoinedNotify,
      signal
    );
    const joinedAgent2 = wait<ParticipantJoinedNotify>(
      agent,
      PacketNames.participantJoinedNotify,
      signal
    );
    const agentJoin2 = await request<JoinConversationRes>(
      agent,
      PacketNames.joinConversationReq,
      joinConversation(cid2),
      undefined,
      signal
    );
    zlinkStreamAssert.ensure(agentJoin2.scheduled, 'Sample scenario assertion failed.');
    const agentRoom2 = requireActor(agent, agentJoin2.actorId);
    zlinkStreamAssert.ensure(
      agentRoom2.actorId !== agentRoom1.actorId,
      'Sample scenario assertion failed.'
    );
    zlinkStreamAssert.ensure(
      agentJoin2.state.status === ConversationStatuses.WaitingForAgent,
      'Sample scenario assertion failed.'
    );
    const customer2Joined = await joinedCustomer2;
    const agent2Joined = await joinedAgent2;
    zlinkStreamAssert.ensure(
      agent2Joined.actorId === agentRoom2.actorId && agent2Joined.actorId !== agent1Joined.actorId,
      'Sample scenario assertion failed.'
    );
    zlinkStreamAssert.ensure(
      customer2Joined.payload.conversationId === cid2,
      'Sample scenario assertion failed.'
    );
    zlinkStreamAssert.ensure(
      customer2Joined.payload.actorId === 'agent-1',
      'Sample scenario assertion failed.'
    );
    zlinkStreamAssert.ensure(
      customer2Joined.payload.role === SupportChatRoles.Agent,
      'Sample scenario assertion failed.'
    );
    zlinkStreamAssert.ensure(
      customer2Joined.payload.state.status === ConversationStatuses.Active,
      'Sample scenario assertion failed.'
    );
    const greeting2Task = wait<ChatMessageNotify>(customer2, PacketNames.chatMessageNotify, signal);
    const greeting2 = await request<SendChatMessageRes>(
      agent,
      PacketNames.sendChatMessageReq,
      sendChatMessage('Let me check your account.'),
      agentRoom2.actorId,
      signal
    );
    zlinkStreamAssert.ensure(
      greeting2.message.messageSeq === 1,
      'Sample scenario assertion failed.'
    );
    zlinkStreamAssert.ensure(
      greeting2.message.conversationId === cid2,
      'Sample scenario assertion failed.'
    );
    zlinkStreamAssert.ensure(
      greeting2.message.senderActorId === 'agent-1',
      'Sample scenario assertion failed.'
    );
    zlinkStreamAssert.ensure(
      greeting2.message.text === 'Let me check your account.',
      'Sample scenario assertion failed.'
    );
    const greeting2Push = await greeting2Task;
    zlinkStreamAssert.ensure(
      greeting2Push.payload.message.messageSeq === 1,
      'Sample scenario assertion failed.'
    );
    zlinkStreamAssert.ensure(
      greeting2Push.payload.conversationId === cid2,
      'Sample scenario assertion failed.'
    );
    zlinkStreamAssert.ensure(
      greeting2Push.payload.message.conversationId === cid2,
      'Sample scenario assertion failed.'
    );
    zlinkStreamAssert.ensure(
      greeting2Push.payload.message.senderActorId === 'agent-1',
      'Sample scenario assertion failed.'
    );
    zlinkStreamAssert.ensure(
      greeting2Push.payload.message.text === 'Let me check your account.',
      'Sample scenario assertion failed.'
    );

    await customer3.connect(signal);
    await request<AuthenticateRes>(
      customer3,
      PacketNames.authenticateReq,
      authenticate('customer-3'),
      undefined,
      signal
    );
    const assigned3Task = wait<ConversationAssignedNotify>(
      agent,
      PacketNames.conversationAssignedNotify,
      signal
    );
    const opened3 = await request<OpenConversationRes>(
      customer3,
      PacketNames.openConversationReq,
      openConversation('refund delayed'),
      undefined,
      signal
    );
    const cid3 = opened3.conversationId;
    const assigned3 = await assigned3Task;
    zlinkStreamAssert.ensure(
      assigned3.payload.conversationId === cid3,
      'Sample scenario assertion failed.'
    );
    const joinedCustomer3 = wait<ParticipantJoinedNotify>(
      customer3,
      PacketNames.participantJoinedNotify,
      signal
    );
    const agentJoin3 = await request<JoinConversationRes>(
      agent,
      PacketNames.joinConversationReq,
      joinConversation(cid3),
      undefined,
      signal
    );
    zlinkStreamAssert.ensure(agentJoin3.scheduled, 'Sample scenario assertion failed.');
    const agentRoom3 = requireActor(agent, agentJoin3.actorId);
    zlinkStreamAssert.ensure(
      agentJoin3.state.status === ConversationStatuses.WaitingForAgent,
      'Sample scenario assertion failed.'
    );
    await joinedCustomer3;
    const greeting3Task = wait<ChatMessageNotify>(customer3, PacketNames.chatMessageNotify, signal);
    const greeting3 = await request<SendChatMessageRes>(
      agent,
      PacketNames.sendChatMessageReq,
      sendChatMessage('I will check the refund.'),
      agentRoom3.actorId,
      signal
    );
    zlinkStreamAssert.ensure(
      greeting3.message.messageSeq === 1,
      'Sample scenario assertion failed.'
    );
    await greeting3Task;

    await customer4.connect(signal);
    await zlinkStreamAssert.expectFailure(async () => {
      await request(
        customer4,
        PacketNames.openConversationReq,
        openConversation('before auth'),
        undefined,
        signal
      );
    });
    await zlinkStreamAssert.expectFailure(async () => {
      await request(
        customer4,
        PacketNames.sendChatMessageReq,
        sendChatMessage('before auth'),
        undefined,
        signal
      );
    });
    send(customer4, PacketNames.setTypingMsg, setTyping(true));
    await customer1.expectNone(PacketNames.typingChangedNotify).within(250).run(signal);
    await request<AuthenticateRes>(
      customer4,
      PacketNames.authenticateReq,
      authenticate('customer-4'),
      undefined,
      signal
    );
    const atCapacity = await request<OpenConversationRes>(
      customer4,
      PacketNames.openConversationReq,
      openConversation('capacity wait'),
      undefined,
      signal
    );
    zlinkStreamAssert.ensure(
      atCapacity.state.status === ConversationStatuses.WaitingForAgent,
      'Sample scenario assertion failed.'
    );
    await agent.expectNone(PacketNames.conversationAssignedNotify).within(250).run(signal);

    zlinkStreamAssert.ensure(
      customer2.actor(customer1Auth.actorId) === undefined,
      'Sample scenario assertion failed.'
    );
    send(customer2, PacketNames.setTypingMsg, setTyping(true));
    await customer1.expectNone(PacketNames.typingChangedNotify).within(250).run(signal);
    const typingTask = wait<TypingChangedNotify>(
      customer1,
      PacketNames.typingChangedNotify,
      signal
    );
    send(agent, PacketNames.setTypingMsg, setTyping(true), agentRoom1.actorId);
    const typing = await typingTask;
    zlinkStreamAssert.ensure(
      typing.payload.actorId === 'agent-1',
      'Sample scenario assertion failed.'
    );

    const reconnectKeepaliveTask = waitConversation<ChatMessageNotify>(
      agent,
      PacketNames.chatMessageNotify,
      agentRoom1.actorId,
      signal
    );
    const reconnectKeepalive = await request<SendChatMessageRes>(
      customer1,
      PacketNames.sendChatMessageReq,
      sendChatMessage('Still looking into it.'),
      undefined,
      signal
    );
    zlinkStreamAssert.ensure(
      reconnectKeepalive.message.messageSeq === 3,
      'Sample scenario assertion failed.'
    );
    zlinkStreamAssert.ensure(
      reconnectKeepalive.state.status === ConversationStatuses.Active,
      'Sample scenario assertion failed.'
    );
    const reconnectKeepalivePush = await reconnectKeepaliveTask;
    zlinkStreamAssert.ensure(
      reconnectKeepalivePush.payload.message.messageSeq === 3,
      'Sample scenario assertion failed.'
    );

    await customer1.close(signal);
    await reconnectedCustomer.connect(signal);
    const reconnectedCustomerAuth = await request<AuthenticateRes>(
      reconnectedCustomer,
      PacketNames.authenticateReq,
      authenticate('customer-1'),
      undefined,
      signal
    );
    zlinkStreamAssert.ensure(
      reconnectedCustomerAuth.actorId === 'customer-1',
      'Sample scenario assertion failed.'
    );
    const customerRejoined1 = await request<JoinConversationRes>(
      reconnectedCustomer,
      PacketNames.joinConversationReq,
      joinConversation(cid1),
      undefined,
      signal
    );
    zlinkStreamAssert.ensure(!customerRejoined1.scheduled, 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(
      customerRejoined1.actorId === reconnectedCustomerAuth.actorId,
      'Sample scenario assertion failed.'
    );
    zlinkStreamAssert.ensure(
      customerRejoined1.state.subject === 'checkout payment failed' &&
        customerRejoined1.state.lastMessageSeq === 3,
      'Sample scenario assertion failed.'
    );

    await agent.close(signal);
    await reconnectedAgent.connect(signal);
    await request<AuthenticateRes>(
      reconnectedAgent,
      PacketNames.authenticateReq,
      authenticate('agent-1'),
      undefined,
      signal
    );
    const reconnectedAvailable = await request<SetAgentAvailableRes>(
      reconnectedAgent,
      PacketNames.setAgentAvailableReq,
      setAgentAvailable(true),
      undefined,
      signal
    );
    zlinkStreamAssert.ensure(reconnectedAvailable.isAvailable, 'Sample scenario assertion failed.');
    const rejoined1 = await request<JoinConversationRes>(
      reconnectedAgent,
      PacketNames.joinConversationReq,
      joinConversation(cid1),
      undefined,
      signal
    );
    const rejoined2 = await request<JoinConversationRes>(
      reconnectedAgent,
      PacketNames.joinConversationReq,
      joinConversation(cid2),
      undefined,
      signal
    );
    const rejoined3 = await request<JoinConversationRes>(
      reconnectedAgent,
      PacketNames.joinConversationReq,
      joinConversation(cid3),
      undefined,
      signal
    );
    zlinkStreamAssert.ensure(
      !rejoined1.scheduled && !rejoined2.scheduled && !rejoined3.scheduled,
      'Sample scenario assertion failed.'
    );
    const reconnectedRoom1 = requireActor(reconnectedAgent, rejoined1.actorId);
    const reconnectedRoom2 = requireActor(reconnectedAgent, rejoined2.actorId);
    const reconnectedRoom3 = requireActor(reconnectedAgent, rejoined3.actorId);
    zlinkStreamAssert.ensure(
      reconnectedRoom1.actorId === agentRoom1.actorId &&
        reconnectedRoom2.actorId === agentRoom2.actorId &&
        reconnectedRoom3.actorId === agentRoom3.actorId,
      'Sample scenario assertion failed.'
    );
    zlinkStreamAssert.ensure(
      rejoined1.state.subject === 'checkout payment failed' && rejoined1.state.lastMessageSeq === 3,
      'Sample scenario assertion failed.'
    );
    zlinkStreamAssert.ensure(
      rejoined2.state.subject === 'cannot log in' && rejoined2.state.lastMessageSeq === 1,
      'Sample scenario assertion failed.'
    );
    zlinkStreamAssert.ensure(
      rejoined3.state.subject === 'refund delayed' && rejoined3.state.lastMessageSeq === 1,
      'Sample scenario assertion failed.'
    );

    const firstIdleCustomer = waitConversation<ConversationIdleNotify>(
      reconnectedCustomer,
      PacketNames.conversationIdleNotify,
      undefined,
      signal
    );
    const firstIdleAgent = waitConversation<ConversationIdleNotify>(
      reconnectedAgent,
      PacketNames.conversationIdleNotify,
      reconnectedRoom1.actorId,
      signal
    );
    const closed2Notify = waitConversation<ConversationClosedNotify>(
      customer2,
      PacketNames.conversationClosedNotify,
      undefined,
      signal
    );
    const closed2 = await request<CloseConversationRes>(
      reconnectedAgent,
      PacketNames.closeConversationReq,
      closeConversation('resolved'),
      reconnectedRoom2.actorId,
      signal
    );
    zlinkStreamAssert.ensure(
      closed2.state.status === ConversationStatuses.Closed,
      'Sample scenario assertion failed.'
    );
    const closed2Push = await closed2Notify;
    zlinkStreamAssert.ensure(
      closed2Push.payload.state.status === ConversationStatuses.Closed,
      'Sample scenario assertion failed.'
    );
    await zlinkStreamAssert.expectFailure(async () => {
      await request(
        customer2,
        PacketNames.closeConversationReq,
        closeConversation(),
        undefined,
        signal
      );
    });

    const [firstCustomerIdle, firstAgentIdle] = await Promise.all([
      firstIdleCustomer,
      firstIdleAgent
    ]);
    zlinkStreamAssert.ensure(
      firstCustomerIdle.payload.state.status === ConversationStatuses.WaitingForClose,
      'Sample scenario assertion failed.'
    );
    zlinkStreamAssert.ensure(
      firstAgentIdle.payload.state.status === ConversationStatuses.WaitingForClose,
      'Sample scenario assertion failed.'
    );
    const resumedPush = waitConversation<ChatMessageNotify>(
      reconnectedAgent,
      PacketNames.chatMessageNotify,
      reconnectedRoom1.actorId,
      signal
    );
    const secondIdleCustomer = waitConversation<ConversationIdleNotify>(
      reconnectedCustomer,
      PacketNames.conversationIdleNotify,
      undefined,
      signal
    );
    const secondIdleAgent = waitConversation<ConversationIdleNotify>(
      reconnectedAgent,
      PacketNames.conversationIdleNotify,
      reconnectedRoom1.actorId,
      signal
    );
    const idleClosedCustomer = waitConversation<ConversationClosedNotify>(
      reconnectedCustomer,
      PacketNames.conversationClosedNotify,
      undefined,
      signal
    );
    const idleClosedAgent = waitConversation<ConversationClosedNotify>(
      reconnectedAgent,
      PacketNames.conversationClosedNotify,
      reconnectedRoom1.actorId,
      signal
    );
    const resumed = await request<SendChatMessageRes>(
      reconnectedCustomer,
      PacketNames.sendChatMessageReq,
      sendChatMessage('I am still here.'),
      undefined,
      signal
    );
    zlinkStreamAssert.ensure(
      resumed.state.status === ConversationStatuses.Active,
      'Sample scenario assertion failed.'
    );
    const resumedNotification = await resumedPush;
    zlinkStreamAssert.ensure(
      resumedNotification.payload.message.messageSeq === 4,
      'Sample scenario assertion failed.'
    );

    await Promise.all([secondIdleCustomer, secondIdleAgent]);
    const [customerIdleClosed, agentIdleClosed] = await Promise.all([
      idleClosedCustomer,
      idleClosedAgent
    ]);
    zlinkStreamAssert.ensure(
      customerIdleClosed.payload.state.status === ConversationStatuses.Closed,
      'Sample scenario assertion failed.'
    );
    zlinkStreamAssert.ensure(
      agentIdleClosed.payload.state.status === ConversationStatuses.Closed,
      'Sample scenario assertion failed.'
    );
    await zlinkStreamAssert.expectFailure(async () => {
      await request(
        reconnectedCustomer,
        PacketNames.sendChatMessageReq,
        sendChatMessage('too late'),
        undefined,
        signal
      );
    });
    send(reconnectedCustomer, PacketNames.setTypingMsg, setTyping(true));
    await reconnectedAgent.expectNone(PacketNames.typingChangedNotify).within(250).run(signal);
    console.log('supportchat-closed-typing-ignore=verified');

    await customer5.connect(signal);
    await request<AuthenticateRes>(
      customer5,
      PacketNames.authenticateReq,
      authenticate('customer-5'),
      undefined,
      signal
    );
    await zlinkStreamAssert.expectFailure(async () => {
      await request(
        customer5,
        PacketNames.setAgentAvailableReq,
        setAgentAvailable(true),
        undefined,
        signal
      );
    });
    const recoveredAssignmentTask = wait<ConversationAssignedNotify>(
      reconnectedAgent,
      PacketNames.conversationAssignedNotify,
      signal
    );
    const capacityRecovered = await request<OpenConversationRes>(
      customer5,
      PacketNames.openConversationReq,
      openConversation('capacity recovered'),
      undefined,
      signal
    );
    const recoveredAssignment = await recoveredAssignmentTask;
    zlinkStreamAssert.ensure(
      recoveredAssignment.payload.conversationId === capacityRecovered.conversationId,
      'Sample scenario assertion failed.'
    );

    const unavailable = await request<SetAgentAvailableRes>(
      reconnectedAgent,
      PacketNames.setAgentAvailableReq,
      setAgentAvailable(false),
      undefined,
      signal
    );
    zlinkStreamAssert.ensure(!unavailable.isAvailable, 'Sample scenario assertion failed.');
    await customer6.connect(signal);
    await request<AuthenticateRes>(
      customer6,
      PacketNames.authenticateReq,
      authenticate('customer-6'),
      undefined,
      signal
    );
    const noAgent = await request<OpenConversationRes>(
      customer6,
      PacketNames.openConversationReq,
      openConversation('agent unavailable'),
      undefined,
      signal
    );
    zlinkStreamAssert.ensure(
      noAgent.state.status === ConversationStatuses.WaitingForAgent,
      'Sample scenario assertion failed.'
    );
    zlinkStreamAssert.ensure(
      noAgent.state.subject === 'agent unavailable',
      'Sample scenario assertion failed.'
    );
    await customer6.expectNone(PacketNames.conversationClosedNotify).within(250).run(signal);
  }
}

function request<T>(
  client: ZlinkStreamConnector,
  packetName: string,
  payload: unknown,
  actorId?: string,
  signal?: AbortSignal
): Promise<T> {
  return (actorId === undefined ? client : requireActor(client, actorId))
    .request(payload, Object)
    .packetName(packetName)
    .submit<T>(signal);
}
function send(
  client: ZlinkStreamConnector,
  packetName: string,
  payload: unknown,
  actorId?: string
): void {
  (actorId === undefined ? client : requireActor(client, actorId))
    .send(payload, Object)
    .packetName(packetName)
    .submit();
}
function requireActor(client: ZlinkStreamConnector, actorId: string) {
  const actor = client.actor(actorId);
  if (actor === undefined) throw new Error(`Bound actor '${actorId}' was not found.`);
  return actor;
}
function wait<T>(client: ZlinkStreamConnector, packetName: string, signal?: AbortSignal) {
  return client.waitFor<T>(packetName).submit(signal);
}
function waitConversation<T>(
  client: ZlinkStreamConnector,
  packetName: string,
  actorId: string | undefined,
  signal?: AbortSignal
) {
  const call = client.waitFor<T>(packetName);
  if (actorId !== undefined) call.where((message) => message.actorId === actorId);
  return call.submit(signal);
}
export { SupportChatClientScenario };
