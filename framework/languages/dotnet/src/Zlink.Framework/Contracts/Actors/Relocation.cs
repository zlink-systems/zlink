namespace Zlink.Framework.Contracts.Actors;

public interface IZLinkActorRelocationAdapter<TActor>
    where TActor : class, IZLinkActor
{
    ValueTask<byte[]> CaptureAsync(
        TActor actor,
        CancellationToken cancellationToken);

    ValueTask RestoreAsync(
        TActor actor,
        ReadOnlyMemory<byte> payload,
        CancellationToken cancellationToken);
}

/// <summary>
/// Optional base/delta relocation capability. When the adapter registered via
/// PreserveStateWith implements this interface, the framework captures a base
/// snapshot at a turn boundary before the relocation seal and sends it ahead,
/// then transfers only the delta captured after the seal. If ApplyDeltaAsync
/// fails, the target instance is discarded and a new instance restarts from
/// RestoreBaseAsync; a partially applied instance is never reused.
/// </summary>
public interface IZLinkActorBaseDeltaRelocationAdapter<TActor>
    : IZLinkActorRelocationAdapter<TActor>
    where TActor : class, IZLinkActor
{
    ValueTask<byte[]> CaptureBaseAsync(
        TActor actor,
        CancellationToken cancellationToken);

    ValueTask<byte[]> CaptureDeltaAsync(
        TActor actor,
        CancellationToken cancellationToken);

    ValueTask RestoreBaseAsync(
        TActor actor,
        ReadOnlyMemory<byte> basePayload,
        CancellationToken cancellationToken);

    ValueTask ApplyDeltaAsync(
        TActor actor,
        ReadOnlyMemory<byte> deltaPayload,
        CancellationToken cancellationToken);
}
