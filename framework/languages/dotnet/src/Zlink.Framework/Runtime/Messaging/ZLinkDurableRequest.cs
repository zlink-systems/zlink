namespace Zlink.Framework.Runtime.Messaging;

// Lifecycle callers wait for initial readiness; the binding owns the started request.
internal static class ZLinkDurableRequest
{
    private static readonly Task CancellationOnly = new TaskCompletionSource().Task;

    internal static async ValueTask<TReply> RequestAsync<TReply>(
        IReadOnlyList<ReadOnlyMemory<byte>> wire,
        long startTimestamp,
        TimeSpan timeout,
        Func<
            IReadOnlyList<ReadOnlyMemory<byte>>,
            TimeSpan,
            CancellationToken,
            ValueTask<TReply>
        > submit,
        CancellationToken cancellationToken,
        Func<ZLinkPeerRouteObservation> observeRoute,
        TimeProvider? timeProvider = null,
        ZLinkRequestAdmission? requestAdmission = null,
        Task? terminal = null
    )
    {
        timeProvider ??= TimeProvider.System;
        while (true)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var remaining = timeout - timeProvider.GetElapsedTime(startTimestamp);
            if (remaining <= TimeSpan.Zero)
            {
                // The entry's owner confirms expiration. A consumed stage budget
                // prevents another submit but does not independently take terminal authority.
                if (terminal is not null)
                {
                    await terminal.ConfigureAwait(false);
                    // Only the original entry can settle the caller; stop this producer.
                    throw new OperationCanceledException(cancellationToken);
                }
                else if (cancellationToken.CanBeCanceled)
                    await CancellationOnly.WaitAsync(cancellationToken).ConfigureAwait(false);
                throw Exhausted(requestAdmission?.IsAdmitted == true);
            }
            var route = observeRoute();
            if (route.ReadyGeneration == 0)
            {
                try
                {
                    var wait = route.Changed.WaitAsync(cancellationToken);
                    if (terminal is null)
                        await wait.ConfigureAwait(false);
                    else
                    {
                        var ready = await Task.WhenAny(wait, terminal).ConfigureAwait(false);
                        await ready.ConfigureAwait(false);
                        if (ReferenceEquals(ready, terminal))
                            throw new OperationCanceledException(cancellationToken);
                    }
                }
                catch (OperationCanceledException)
                {
                    await Task.CompletedTask.ConfigureAwait(ConfigureAwaitOptions.ForceYielding);
                    throw;
                }
                continue;
            }
            return await submit(wire, remaining, cancellationToken).ConfigureAwait(false);
        }
    }

    internal static bool CanReplay(Exception error, out bool admitted)
    {
        if (error is ZLinkFrameworkException { InnerException: { } cause })
            error = cause;
        admitted = error is ZlinkRequestException;
        return error
            is ZlinkRequestException
                {
                    Result: ZlinkRequestException.ErrorCode.NotConnected
                        or ZlinkRequestException.ErrorCode.TimedOut
                }
                or ZlinkSubmitException
                {
                    Result: ZlinkSubmitException.ErrorCode.NotConnected
                        or ZlinkSubmitException.ErrorCode.NotFound
                        or ZlinkSubmitException.ErrorCode.NotAdmitted
                };
    }

    private static ZLinkFrameworkException Exhausted(bool admitted) =>
        new(
            admitted
                ? ZLinkFrameworkErrorKind.DeadlineExceeded
                : ZLinkFrameworkErrorKind.Unavailable,
            admitted
                ? "Durable request reply was not received before its deadline."
                : "Durable request was not admitted before its deadline.",
            ZLinkRetryAdvice.RetryAfterBackoff
        );
}

// MeshNode owns the selected identity and readiness; waiting callers use its topology edge.
internal readonly record struct ZLinkPeerRouteObservation(
    ulong SelectedGeneration,
    ulong ReadyGeneration,
    Task Changed
);
