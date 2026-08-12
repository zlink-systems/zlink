using Zlink.Framework.Contracts.Handlers;
using SupportChat.Server.Support.Infrastructure.ZLink.Actors;
using SupportChat.Shared.Contracts;
using Zlink.Framework.Contracts.Spots;

namespace SupportChat.Server.Support.Infrastructure.ZLink.Spots.ConversationSpot.Handlers;

// Handles JoinConversationReq from an actor that is already a member (for
// example after reconnect) and returns the current state to the requesting
// session.
internal sealed class JoinConversationHandler
    : IZLinkSpotActorRequestHandler<
        ConversationSpot,
        SupportUserActor,
        JoinConversationReq,
        JoinConversationRes>
{
    public ValueTask<JoinConversationRes> HandleAsync(
        ConversationSpot spot,
        SupportUserActor actor,
        IZLinkMessageContext context,
        JoinConversationReq message,
        CancellationToken cancellationToken)
    {
        _ = context;
        _ = message;
        cancellationToken.ThrowIfCancellationRequested();
        return ValueTask.FromResult(spot.RefreshMembership(actor));
    }
}
