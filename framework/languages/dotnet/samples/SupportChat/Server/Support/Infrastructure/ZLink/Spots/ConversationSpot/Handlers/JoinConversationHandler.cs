using Zlink.Framework.Contracts.Handlers;
using SupportChat.Server.Support.Infrastructure.ZLink.Actors;
using SupportChat.Shared.Contracts;
using Zlink.Framework.Contracts.Spots;

namespace SupportChat.Server.Support.Infrastructure.ZLink.Spots.ConversationSpot.Handlers;

// Handles the one-way JoinConversationReq from an actor that is already a
// member (for example after a reconnect) and pushes the current state to the
// actor's newly bound session.
internal sealed class JoinConversationHandler
    : IZLinkSpotActorSendHandler<ConversationSpot, SupportUserActor, JoinConversationReq>
{
    public async ValueTask HandleAsync(
        ConversationSpot spot,
        SupportUserActor actor,
        IZLinkMessageContext context,
        JoinConversationReq message,
        CancellationToken cancellationToken)
    {
        _ = context;
        _ = message;
        var result = spot.RefreshMembership(actor);
        await actor.Context.BoundSession
            .Send(result)
            .Async(cancellationToken);
    }
}
