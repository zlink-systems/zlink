namespace SupportChat.Shared.Contracts;

public sealed record AuthenticateReq(string AccessToken);

public sealed record AuthenticateRes(
    string ActorId,
    string DisplayName,
    string Role);

public sealed record AuthenticateUserReq(string AccessToken);

public sealed record AuthenticateUserRes(
    bool Accepted,
    string? ActorId,
    string? DisplayName,
    string? Role,
    string? Reason);

public sealed record OpenConversationApiReq(
    string CustomerActorId,
    string CustomerDisplayName,
    string Subject);

public sealed record OpenConversationApiRes(
    ConversationState State);

public sealed record ConversationCreateReq(
    string CustomerActorId,
    string CustomerDisplayName,
    string Subject,
    long CreatedAtUnixMs);

public sealed record ConversationCreateRes(ConversationState State);

public sealed record OpenConversationReq(string Subject);

public sealed record OpenConversationRes(
    string ConversationId,
    ConversationState State);

public sealed record SetAgentAvailableReq(bool IsAvailable);

public sealed record SetAgentAvailableRes(bool IsAvailable);

// ConversationId travels as stream message metadata (§9.2), not in these bodies.
public sealed record JoinConversationReq(
    string ParticipantId = "",
    string Role = "",
    string DisplayName = "");

public sealed record JoinConversationRes(
    bool Scheduled,
    ConversationState State);

public sealed record JoinConversationFailedNotify(
    string ConversationId,
    string Error);

public sealed record SendChatMessageReq(string Text);

public sealed record SendChatMessageRes(
    ChatMessage Message,
    ConversationState State);

// SetTyping is a one-way fire-and-forget send: no response record.
public sealed record SetTypingMsg(bool IsTyping);

public sealed record CloseConversationReq(string? Reason);

public sealed record CloseConversationRes(ConversationState State);

public sealed record ParticipantJoinedNotify(
    string ConversationId,
    string ActorId,
    string Role,
    ConversationState State);

public sealed record ConversationAssignedNotify(
    string ConversationId,
    ConversationState State);

public sealed record ChatMessageNotify(
    string ConversationId,
    ChatMessage Message,
    ConversationState State);

public sealed record TypingChangedNotify(
    string ConversationId,
    string ActorId,
    bool IsTyping,
    ConversationState State);

public sealed record ConversationIdleNotify(
    string ConversationId,
    ConversationState State);

public sealed record ConversationClosedNotify(
    string ConversationId,
    ConversationState State);

public sealed record ConversationState(
    string ConversationId,
    string Subject,
    string Status,
    string CustomerActorId,
    string? AgentActorId,
    ulong LastMessageSeq,
    long? LastMessageAtUnixMs,
    long? IdleDeadlineUnixMs);

public sealed record ChatMessage(
    string ConversationId,
    ulong MessageSeq,
    string SenderActorId,
    string Text,
    long SentAtUnixMs);
