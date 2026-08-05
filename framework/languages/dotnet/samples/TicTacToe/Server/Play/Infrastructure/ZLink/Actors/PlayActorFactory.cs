using Zlink.Framework.Contracts.Actors;

namespace TicTacToe.Server.Play.Infrastructure.ZLink.Actors;

internal sealed class PlayActorFactory : IZLinkActorFactory<PlayActor>
{
    public ValueTask<PlayActor> CreateAsync(
        IZLinkActorContext context,
        CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        return ValueTask.FromResult(new PlayActor(context.ActorId, context));
    }
}
