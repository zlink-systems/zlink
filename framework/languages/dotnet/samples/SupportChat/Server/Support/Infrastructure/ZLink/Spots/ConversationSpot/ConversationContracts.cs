using SupportChat.Server.Configuration;
using SupportChat.Server.Support.Domain.SupportChat;
using SupportChat.Shared.Contracts;

namespace SupportChat.Server.Support.Infrastructure.ZLink.Spots.ConversationSpot;

// Adapter boundary: maps pure domain snapshots/messages/roles to the transport wire
// contracts (§7 — the domain never references transport types; adapters translate).
internal static class ConversationContracts
{
    public static ConversationState ToState(ConversationSnapshot snapshot)
    {
        return new ConversationState(
            snapshot.ConversationId,
            snapshot.Subject,
            ToStatus(snapshot.Status),
            snapshot.CustomerActorId,
            snapshot.AgentActorId,
            snapshot.LastMessageSeq,
            snapshot.LastMessageAtUnixMs,
            snapshot.IdleDeadlineUnixMs);
    }

    public static ChatMessage ToMessage(ConversationMessage message)
    {
        return new ChatMessage(
            message.ConversationId,
            message.MessageSeq,
            message.SenderActorId,
            message.Text,
            message.SentAtUnixMs);
    }

    public static string ToStatus(ConversationStatus status)
    {
        return status switch
        {
            ConversationStatus.WaitingForAgent => ConversationStatuses.WaitingForAgent,
            ConversationStatus.Active => ConversationStatuses.Active,
            ConversationStatus.WaitingForClose => ConversationStatuses.WaitingForClose,
            ConversationStatus.Closed => ConversationStatuses.Closed,
            _ => throw new InvalidOperationException($"Unknown conversation status {status}.")
        };
    }

    public static string ToRole(ParticipantRole role)
    {
        return role switch
        {
            ParticipantRole.Customer => SupportChatRoles.Customer,
            ParticipantRole.Agent => SupportChatRoles.Agent,
            _ => throw new InvalidOperationException($"Unknown participant role {role}.")
        };
    }
}
