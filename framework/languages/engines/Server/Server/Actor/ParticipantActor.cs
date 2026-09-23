using Zlink.Framework.Contracts.Actors;

namespace EngineLobby.Server;

public sealed class ParticipantActor(IZLinkActorContext context) : IZLinkActor
{
    public IZLinkActorContext Context { get; } = context;

    public string Name { get; private set; } = string.Empty;

    public void SetName(string name) => Name = name;
}

public sealed class ParticipantActorFactory : IZLinkActorFactory<ParticipantActor>
{
    public ValueTask<ParticipantActor> CreateAsync(
        IZLinkActorContext context,
        CancellationToken cancellationToken = default
    ) => ValueTask.FromResult(new ParticipantActor(context));

    async ValueTask<IZLinkActor> IZLinkActorFactory.CreateAsync(
        IZLinkActorContext context,
        CancellationToken cancellationToken
    ) => await CreateAsync(context, cancellationToken);
}
