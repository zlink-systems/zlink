namespace SupportChat.Server.Support.Domain.SupportChat;

internal enum ConversationStatus
{
    WaitingForAgent,
    Active,
    WaitingForClose,
    Closed
}

internal enum ParticipantRole
{
    Customer,
    Agent
}

internal sealed record ConversationPolicy(
    TimeSpan IdleTimeout,
    TimeSpan CloseGraceTimeout,
    int MaxMessageLength)
{
    public static ConversationPolicy Sample { get; } = new(
        TimeSpan.FromSeconds(3),
        TimeSpan.FromSeconds(2),
        500);
}

internal sealed record ConversationParticipant(
    string ActorId,
    ParticipantRole Role,
    string DisplayName,
    long JoinedAtUnixMs,
    bool IsTyping);

internal sealed record ConversationMessage(
    string ConversationId,
    ulong MessageSeq,
    string SenderActorId,
    string Text,
    long SentAtUnixMs);

// Domain-owned view of a conversation's state. Infrastructure adapters map this to the
// ConversationState wire contract.
internal sealed record ConversationSnapshot(
    string ConversationId,
    string Subject,
    ConversationStatus Status,
    string CustomerActorId,
    string? AgentActorId,
    ulong LastMessageSeq,
    long? LastMessageAtUnixMs,
    long? IdleDeadlineUnixMs);

internal enum ConversationEventKind
{
    ParticipantJoined,
    MessageAppended,
    TypingChanged,
    Idle,
    Closed
}

internal sealed record ConversationEvent(
    ConversationEventKind Kind,
    ConversationSnapshot State,
    string? ActorId = null,
    ParticipantRole? Role = null,
    ConversationMessage? Message = null,
    bool? IsTyping = null);

internal sealed record ConversationChange(
    ConversationSnapshot State,
    IReadOnlyList<ConversationEvent> Events);
