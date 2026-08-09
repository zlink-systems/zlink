namespace Zlink.Framework.UnitTests;

internal sealed class TestActor(string actorId) : IZLinkActor
{
    public string ActorId { get; } = actorId;

    public IZLinkActorContext Context { get; } = new TestActorContext(actorId);
}
