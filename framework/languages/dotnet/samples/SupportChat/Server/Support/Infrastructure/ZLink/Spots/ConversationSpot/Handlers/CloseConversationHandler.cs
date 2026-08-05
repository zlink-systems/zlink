using Zlink.Framework.Contracts.Handlers;
using SupportChat.Server.Support.Infrastructure.ZLink.Actors;
using SupportChat.Shared.Contracts;
using Zlink.Framework.Contracts.Spots;

namespace SupportChat.Server.Support.Infrastructure.ZLink.Spots.ConversationSpot.Handlers;

internal sealed class CloseConversationHandler
    : IZLinkSpotActorRequestHandler<ConversationSpot, SupportUserActor, CloseConversationReq, CloseConversationRes>
{
    public async ValueTask<CloseConversationRes> HandleAsync(
        ConversationSpot spot,
        SupportUserActor actor,
        IZLinkMessageContext context,
        CloseConversationReq message,
        CancellationToken cancellationToken)
    {
        return await spot.CloseAsync(actor, message, cancellationToken);
    }
}
