using Zlink.Framework.Contracts.Handlers;
using SupportChat.Server.Support.Infrastructure.ZLink.Actors;
using SupportChat.Shared.Contracts;
using Zlink.Framework.Contracts.Spots;

namespace SupportChat.Server.Support.Infrastructure.ZLink.Spots.ConversationSpot.Handlers;

// Typing is a one-way fire-and-forget send: the sender gets no response and only
// the other participant receives a TypingChangedNotify push.
internal sealed class SetTypingHandler
    : IZLinkSpotActorSendHandler<ConversationSpot, SupportUserActor, SetTypingMsg>
{
    public async ValueTask HandleAsync(
        ConversationSpot spot,
        SupportUserActor actor,
        IZLinkMessageContext context,
        SetTypingMsg message,
        CancellationToken cancellationToken)
    {
        await spot.SetTypingAsync(actor, message, cancellationToken);
    }
}
