using Zlink.Framework.Contracts.Actors;

namespace Tutorial.Server.Actors;

// --8<-- [start:actor-class]
// A player is addressed by its own id and carries state that outlives any one
// connection. Like a room, its messages run one at a time.
public sealed class Player(IZLinkActorContext context) : IZLinkActor
{
    public IZLinkActorContext Context { get; } = context;

    public string Nickname { get; private set; } = "anonymous";

    public void Rename(string nickname) => Nickname = nickname;
}

// --8<-- [end:actor-class]

// --8<-- [start:actor-factory]
// The Framework creates players through this factory rather than by calling a
// constructor, so dependencies can be injected here.
public sealed class PlayerFactory : IZLinkActorFactory<Player>
{
    public ValueTask<Player> CreateAsync(
        IZLinkActorContext context,
        CancellationToken cancellationToken = default
    ) => ValueTask.FromResult(new Player(context));

    async ValueTask<IZLinkActor> IZLinkActorFactory.CreateAsync(
        IZLinkActorContext context,
        CancellationToken cancellationToken
    ) => await CreateAsync(context, cancellationToken);
}
// --8<-- [end:actor-factory]
