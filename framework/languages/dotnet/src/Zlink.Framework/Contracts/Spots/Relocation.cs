namespace Zlink.Framework.Contracts.Spots;

public interface IZLinkSpotRelocationAdapter<TSpot>
    where TSpot : class
{
    ValueTask<byte[]> CaptureAsync(
        TSpot spot,
        CancellationToken cancellationToken);

    ValueTask RestoreAsync(
        TSpot spot,
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
public interface IZLinkSpotBaseDeltaRelocationAdapter<TSpot>
    : IZLinkSpotRelocationAdapter<TSpot>
    where TSpot : class
{
    ValueTask<byte[]> CaptureBaseAsync(
        TSpot spot,
        CancellationToken cancellationToken);

    ValueTask<byte[]> CaptureDeltaAsync(
        TSpot spot,
        CancellationToken cancellationToken);

    ValueTask RestoreBaseAsync(
        TSpot spot,
        ReadOnlyMemory<byte> basePayload,
        CancellationToken cancellationToken);

    ValueTask ApplyDeltaAsync(
        TSpot spot,
        ReadOnlyMemory<byte> deltaPayload,
        CancellationToken cancellationToken);
}
