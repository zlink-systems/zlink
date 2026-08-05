namespace Zlink.Framework.Runtime.Streams;

internal static class ZLinkNativeActorStreamBinding
{
    public static async ValueTask BindAsync(
        IZLinkStream stream,
        ZLinkBackendActorRef actorRef,
        TimeSpan timeout,
        CancellationToken cancellationToken)
    {
        if (stream is not ZLinkManagedStream managedStream) return;

        await managedStream.BindActorAsync(actorRef, timeout, cancellationToken)
            .ConfigureAwait(false);
    }

    public static async ValueTask UnbindAsync(
        IZLinkStream stream,
        string actorId,
        TimeSpan timeout,
        CancellationToken cancellationToken)
    {
        if (stream is not ZLinkManagedStream managedStream) return;

        await managedStream.UnbindActorAsync(actorId, timeout, cancellationToken)
            .ConfigureAwait(false);
    }
}
