namespace Zlink.Framework.Runtime.Locations;

/// <summary>A descriptor Store write failed without a confirmed result (Location runtime section 5).</summary>
internal sealed class ZLinkDescriptorPublicationUnconfirmedException(Exception cause)
    : Exception(cause.Message, cause)
{
    internal static async ValueTask<ZLinkLocationWriteResult> WriteAsync(
        Func<CancellationToken, ValueTask<ZLinkLocationWriteResult>> write,
        TimeSpan timeout,
        CancellationToken cancellationToken,
        TimeProvider? timeProvider = null
    )
    {
        using var timeoutSource = new CancellationTokenSource(
            timeout,
            timeProvider ?? TimeProvider.System
        );
        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(
            cancellationToken,
            timeoutSource.Token
        );
        try
        {
            // The provider may ignore cancellation; end observation before any caller
            // can apply a late Store result to coordinator state.
            var result = await write(deadline.Token)
                .AsTask()
                .WaitAsync(deadline.Token)
                .ConfigureAwait(false);
            deadline.Token.ThrowIfCancellationRequested();
            return result;
        }
        catch (Exception error)
            when (ZLinkLocationStoreFailure.IsIndeterminate(error, cancellationToken))
        {
            throw new ZLinkDescriptorPublicationUnconfirmedException(error);
        }
    }
}
