using SupportChat.Server.Configuration;
using SupportChat.Server.Support.Infrastructure.ZLink.Actors;
using SupportChat.Shared.Contracts;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Spots;

namespace SupportChat.Server.Support.Infrastructure.ZLink.Spots.EntrySpot.Handlers;

internal sealed class JoinConversationActorHandler
    : IZLinkEntrySpotActorRequestHandler<
        SupportEntrySpot,
        SupportUserActor,
        JoinConversationReq,
        JoinConversationRes>
{
    public ValueTask<JoinConversationRes> HandleAsync(
        SupportEntrySpot entrySpot,
        SupportUserActor actor,
        IZLinkMessageContext context,
        JoinConversationReq message,
        CancellationToken cancellationToken)
    {
        _ = entrySpot;
        _ = message;
        cancellationToken.ThrowIfCancellationRequested();
        if (!string.Equals(
                actor.Role,
                SupportChatRoles.Agent,
                StringComparison.Ordinal))
        {
            throw new InvalidOperationException(
                "Only agent conversation actors can join through this handler.");
        }

        var conversationId = context.Metadata.Find(
            SampleNames.ConversationIdMetadataKey)
            ?? throw new InvalidOperationException(
                "Conversation Join is missing the ConversationId metadata.");
        actor.TrackDeferredJoin(conversationId, notifyBoundSession: true);
        actor.Context.JoinSpot(
                conversationId,
                new JoinConversationReq(
                    actor.ParticipantId,
                    actor.Role,
                    actor.DisplayName))
            .Defer();
        return ValueTask.FromResult(new JoinConversationRes(
            true,
            new ConversationState(
                conversationId,
                string.Empty,
                ConversationStatuses.WaitingForAgent,
                string.Empty,
                null,
                0,
                null,
                null)));
    }
}
