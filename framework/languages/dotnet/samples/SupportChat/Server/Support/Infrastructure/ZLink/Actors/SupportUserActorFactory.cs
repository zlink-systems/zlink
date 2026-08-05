using Zlink.Framework.Contracts.Actors;

namespace SupportChat.Server.Support.Infrastructure.ZLink.Actors;

internal sealed class SupportUserActorFactory : IZLinkActorFactory<SupportUserActor>
{
    public ValueTask<SupportUserActor> CreateAsync(
        IZLinkActorContext context,
        CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        return ValueTask.FromResult(new SupportUserActor(context.ActorId, context));
    }
}
