using Microsoft.Extensions.Logging;
using Zlink.Framework.Contracts.Actors;

namespace Bingo.Server.Play.Infrastructure.ZLink.Actors;

internal sealed class PlayerActorFactory(
    ILogger<PlayerActor> logger) : IZLinkActorFactory<PlayerActor>
{
    public ValueTask<PlayerActor> CreateAsync(
        IZLinkActorContext context,
        CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        return ValueTask.FromResult(new PlayerActor(context.ActorId, context, logger));
    }
}
