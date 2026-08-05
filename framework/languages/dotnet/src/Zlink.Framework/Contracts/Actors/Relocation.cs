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
