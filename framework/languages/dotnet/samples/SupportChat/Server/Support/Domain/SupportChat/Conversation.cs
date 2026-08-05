namespace SupportChat.Server.Support.Domain.SupportChat;

// Aggregate root: owns all conversation state transitions and validation. Pure domain —
// no framework, transport, or configuration types. Infrastructure adapters map its
// snapshots/messages to wire contracts.
internal sealed class Conversation
{
    private readonly List<ConversationMessage> _messages = [];
    private readonly Dictionary<string, ConversationParticipant> _participants = new(StringComparer.Ordinal);
    private readonly ConversationPolicy _policy;
    private long? _closeDeadlineUnixMs;
    private long? _idleDeadlineUnixMs;
    private long? _lastMessageAtUnixMs;
    private ulong _lastMessageSeq;

    public Conversation(
        string conversationId,
        string subject,
        string customerActorId,
        string customerDisplayName,
        long createdAtUnixMs,
        ConversationPolicy? policy = null)
    {
        if (string.IsNullOrWhiteSpace(subject))
            throw new InvalidOperationException("Conversation subject is required.");

        ConversationId = conversationId;
        Subject = subject;
        CustomerActorId = customerActorId;
        Status = ConversationStatus.WaitingForAgent;
        _policy = policy ?? ConversationPolicy.Sample;
        _participants[customerActorId] = new ConversationParticipant(
            customerActorId,
            ParticipantRole.Customer,
            customerDisplayName,
            createdAtUnixMs,
            false);
    }

    public string ConversationId { get; }

    public string Subject { get; }

    public ConversationStatus Status { get; private set; }

    public string CustomerActorId { get; }

    public string? AgentActorId { get; private set; }

    public ConversationSnapshot Snapshot()
    {
        return new ConversationSnapshot(
            ConversationId,
            Subject,
            Status,
            CustomerActorId,
            AgentActorId,
            _lastMessageSeq,
            _lastMessageAtUnixMs,
            _idleDeadlineUnixMs);
    }

    public ConversationChange JoinAgent(
        string agentActorId,
        string agentDisplayName,
        long joinedAtUnixMs)
    {
        EnsureNotClosed("join an agent");
        if (AgentActorId is not null && !string.Equals(AgentActorId, agentActorId, StringComparison.Ordinal))
            throw new InvalidOperationException("Conversation already has an assigned agent.");

        AgentActorId = agentActorId;
        Status = ConversationStatus.Active;
        _participants[agentActorId] = new ConversationParticipant(
            agentActorId,
            ParticipantRole.Agent,
            agentDisplayName,
            joinedAtUnixMs,
            false);

        var state = Snapshot();
        return new ConversationChange(
            state,
            [
                new ConversationEvent(
                    ConversationEventKind.ParticipantJoined,
                    state,
                    agentActorId,
                    ParticipantRole.Agent)
            ]);
    }

    public ConversationChange SendMessage(
        string senderActorId,
        string text,
        long sentAtUnixMs)
    {
        EnsureParticipant(senderActorId);
        if (Status == ConversationStatus.Closed)
            throw new InvalidOperationException("Closed conversation cannot accept messages.");
        if (Status == ConversationStatus.WaitingForAgent)
            throw new InvalidOperationException("Conversation is waiting for an agent.");
        if (string.IsNullOrWhiteSpace(text)) throw new InvalidOperationException("Message text is required.");
        if (text.Length > _policy.MaxMessageLength) throw new InvalidOperationException("Message text is too long.");

        Status = ConversationStatus.Active;
        _lastMessageSeq += 1;
        _lastMessageAtUnixMs = sentAtUnixMs;
        _idleDeadlineUnixMs = sentAtUnixMs + (long)_policy.IdleTimeout.TotalMilliseconds;
        _closeDeadlineUnixMs = null;
        var message = new ConversationMessage(
            ConversationId,
            _lastMessageSeq,
            senderActorId,
            text,
            sentAtUnixMs);
        _messages.Add(message);

        var state = Snapshot();
        return new ConversationChange(
            state,
            [
                new ConversationEvent(
                    ConversationEventKind.MessageAppended,
                    state,
                    senderActorId,
                    Message: message)
            ]);
    }

    public ConversationChange SetTyping(
        string actorId,
        bool isTyping)
    {
        // Typing is a one-way send: a closed conversation or a non-participant sender
        // is silently ignored (no event, no error) rather than rejected.
        if (Status == ConversationStatus.Closed || !_participants.TryGetValue(actorId, out var participant))
            return new ConversationChange(Snapshot(), []);

        _participants[actorId] = participant with { IsTyping = isTyping };
        var state = Snapshot();
        return new ConversationChange(
            state,
            [
                new ConversationEvent(
                    ConversationEventKind.TypingChanged,
                    state,
                    actorId,
                    participant.Role,
                    IsTyping: isTyping)
            ]);
    }

    // Called on a timer tick. Advances the idle lifecycle one stage at a time:
    // Active past the idle deadline moves to WaitingForClose and arms the grace
    // deadline; WaitingForClose past the grace deadline closes the conversation.
    public ConversationChange MarkIdle(long nowUnixMs)
    {
        if (Status == ConversationStatus.Active
            && _idleDeadlineUnixMs is not null
            && nowUnixMs >= _idleDeadlineUnixMs)
        {
            Status = ConversationStatus.WaitingForClose;
            _closeDeadlineUnixMs = nowUnixMs + (long)_policy.CloseGraceTimeout.TotalMilliseconds;
            var idleState = Snapshot();
            return new ConversationChange(
                idleState,
                [new ConversationEvent(ConversationEventKind.Idle, idleState)]);
        }

        if (Status == ConversationStatus.WaitingForClose
            && _closeDeadlineUnixMs is not null
            && nowUnixMs >= _closeDeadlineUnixMs)
        {
            Status = ConversationStatus.Closed;
            var closedState = Snapshot();
            return new ConversationChange(
                closedState,
                [new ConversationEvent(ConversationEventKind.Closed, closedState)]);
        }

        return new ConversationChange(Snapshot(), []);
    }

    public ConversationChange Close(
        string actorId,
        string? reason)
    {
        _ = reason;
        EnsureParticipant(actorId);
        if (Status == ConversationStatus.Closed)
            throw new InvalidOperationException("Conversation is already closed.");

        Status = ConversationStatus.Closed;
        var state = Snapshot();
        return new ConversationChange(
            state,
            [new ConversationEvent(ConversationEventKind.Closed, state, actorId)]);
    }

    private ConversationParticipant EnsureParticipant(string actorId)
    {
        if (!_participants.TryGetValue(actorId, out var participant))
            throw new InvalidOperationException("Actor is not a conversation participant.");

        return participant;
    }

    private void EnsureNotClosed(string action)
    {
        if (Status == ConversationStatus.Closed)
            throw new InvalidOperationException($"Closed conversation cannot {action}.");
    }
}
