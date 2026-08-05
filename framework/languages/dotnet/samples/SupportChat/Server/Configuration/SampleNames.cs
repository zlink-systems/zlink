using SupportChat.Shared.Contracts;

namespace SupportChat.Server.Configuration;

public static class SampleNames
{
    public const string MeshName = "supportchat";
    public const string ApiChannel = "supportchat.api";
    public const string SupportActorType = "supportchat.user";
    public const string ConversationSpotType = "supportchat.conversation";
    public const string SupportSpotNode = "supportchat.support.node";
    public const string SessionSpotNode = "supportchat.session.node";
    public const string SupportEntrySpotNode = "supportchat.entry.node";
    public const string SupportConversationSpotNode = "supportchat.conversation.node";
    public const string StreamNode = "supportchat.stream";

    // One agent handles up to this many conversations concurrently (§9).
    public const int AgentCapacity = 3;

    // Stream metadata key that carries ConversationId for session routing (§9.2).
    public const string ConversationIdMetadataKey = "ConversationId";

    public const string ParticipantJoinedPacket = nameof(ParticipantJoinedNotify);
    public const string ConversationAssignedPacket = nameof(ConversationAssignedNotify);
    public const string ChatMessagePacket = nameof(ChatMessageNotify);
    public const string TypingChangedPacket = nameof(TypingChangedNotify);
    public const string ConversationIdlePacket = nameof(ConversationIdleNotify);
    public const string ConversationClosedPacket = nameof(ConversationClosedNotify);
}

public static class SampleTimings
{
    public static readonly TimeSpan ConnectTimeout = TimeSpan.FromSeconds(5);
    public static readonly TimeSpan RequestTimeout = TimeSpan.FromSeconds(5);
    public static readonly TimeSpan IdleTimeout = TimeSpan.FromSeconds(3);
    public static readonly TimeSpan CloseGraceTimeout = TimeSpan.FromSeconds(2);
}

public static class SupportChatRoles
{
    public const string Customer = "Customer";
    public const string Agent = "Agent";
}

public static class ConversationStatuses
{
    public const string WaitingForAgent = "WaitingForAgent";
    public const string Active = "Active";
    public const string WaitingForClose = "WaitingForClose";
    public const string Closed = "Closed";
}
