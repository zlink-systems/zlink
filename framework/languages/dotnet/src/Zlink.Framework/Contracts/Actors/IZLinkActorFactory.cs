namespace Zlink.Framework.Contracts.Actors;

public interface IZLinkActorFactory
{
    ValueTask<IZLinkActor> CreateAsync(
        IZLinkActorContext context,
        CancellationToken cancellationToken = default);
}

public interface IZLinkActorFactory<TActor> : IZLinkActorFactory
    where TActor : class, IZLinkActor
{
    new ValueTask<TActor> CreateAsync(
        IZLinkActorContext context,
        CancellationToken cancellationToken = default);

    async ValueTask<IZLinkActor> IZLinkActorFactory.CreateAsync(
        IZLinkActorContext context,
        CancellationToken cancellationToken)
    {
        return await CreateAsync(context, cancellationToken)
            .ConfigureAwait(false);
    }
}
