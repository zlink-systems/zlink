namespace SupportChat.Server.Support.Infrastructure.ZLink.Actors;

internal sealed record SupportActorDirectoryEntry(
    SupportUserActor Actor,
    string DisplayName,
    string Role);

internal sealed class SupportActorDirectory
{
    private readonly Dictionary<string, SupportActorDirectoryEntry> _actors = new(StringComparer.Ordinal);

    public void AddOrUpdate(SupportUserActor actor)
    {
        _actors[actor.ActorId] = new SupportActorDirectoryEntry(
            actor,
            actor.DisplayName,
            actor.Role);
    }

    public SupportActorDirectoryEntry Get(string actorId)
    {
        return _actors.TryGetValue(actorId, out var actor)
            ? actor
            : throw new InvalidOperationException($"Support actor is not available. actor={actorId}");
    }
}
