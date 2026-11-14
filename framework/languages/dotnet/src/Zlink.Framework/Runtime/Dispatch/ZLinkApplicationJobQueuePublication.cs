using Zlink.Framework.Runtime.Execution;

namespace Zlink.Framework.Runtime.Dispatch;

// Local publication crosses from host capacity admission into a destination FIFO.
// The queue owns admission; the destination owns execution and its completion.
internal static class ZLinkApplicationJobQueuePublication
{
    internal static async Task AcquireAndPostAsync(
        this ZLinkApplicationJobQueue queue,
        CancellationToken cancellationToken,
        ZLinkStateLane destination,
        Action<ZLinkApplicationJobQueueLease> publish,
        ZLinkApplicationJobOrigin origin
    )
    {
        var publication = destination.PrepareAsync<ZLinkApplicationJobQueueLease?>(granted =>
        {
            publish(granted!);
            return ValueTask.CompletedTask;
        });
        var reservation = await queue
            .AcquireAsyncCore(1, cancellationToken, publication, origin)
            .ConfigureAwait(false);
        var admission = reservation.Claim()!;
        try
        {
            await publication.Completion.ConfigureAwait(false);
            admission = null;
        }
        finally
        {
            admission?.Dispose();
        }
    }
}
