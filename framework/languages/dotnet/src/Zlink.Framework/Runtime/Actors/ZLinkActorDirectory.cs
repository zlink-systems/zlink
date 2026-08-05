namespace Zlink.Framework.Runtime.Actors;

internal interface IZLinkActorResolver
{
    ValueTask<(ActorRef? Ref, bool RowPresent)> FindWithPresenceAsync(
        string actorId,
        CancellationToken cancellationToken = default);
}

internal sealed class ZLinkActorDirectory(
    ZLinkFrameworkRuntime runtime,
    ZLinkFrameworkRegistration registration,
    ZLinkStoreLocationResolvers? locations = null) : IZLinkActorResolver
{
    private readonly ZLinkSpotMeshLocationResolver? _meshRows = locations is null
        ? null
        : new ZLinkSpotMeshLocationResolver(registration, locations);

    public async ValueTask<ActorRef?> FindAsync(
        string actorId,
        CancellationToken cancellationToken = default)
    {
        var (actorRef, _) = await FindWithPresenceAsync(actorId, cancellationToken)
            .ConfigureAwait(false);
        return actorRef;
    }

    /// <summary>Find plus the raw-row presence: a null ref with a present row
    /// is a transient resolve window (claimed-but-unpublished generation-0,
    /// lagging replica) worth retrying; a null ref without a row is a
    /// confirmed miss — the actor was destroyed or never existed.</summary>
    public async ValueTask<(ActorRef? Ref, bool RowPresent)> FindWithPresenceAsync(
        string actorId,
        CancellationToken cancellationToken = default)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(actorId);

        if (runtime.TryGetCreatedActorState(actorId, out var state)
            && state.NativeActorRef is { } localActorRef)
        {
            var meshName = state.Activation?.MeshName
                           ?? state.Context?.MeshName;
            if (string.IsNullOrWhiteSpace(meshName))
                return (null, true);
            return (localActorRef.ToNative(meshName), true);
        }

        if (locations is null)
        {
            return (null, false);
        }

        var (row, rowPresent) = await _meshRows!.ResolveActorWithPresenceAsync(actorId, cancellationToken)
            .ConfigureAwait(false);
        return (row?.ActorRef, rowPresent);
    }

}
