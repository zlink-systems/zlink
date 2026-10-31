namespace Zlink.Framework.Runtime.Messaging;

// Logical request admission is shared by application and lifecycle requests.
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
        Task? terminal = null
    )
    {
        timeProvider ??= TimeProvider.System;
        ConnectionAttempt? baseline = null;
        var waiting = false;
        string? endpoint = null;
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
                throw Exhausted();
            }
            var route = observeRoute();
            var attempt = route.Attempt;
            // Ready observations do not scan attempts. Only an already waiting
            // request needs the retained endpoint's failure history after recovery.
            if (waiting && route.ReadyGeneration != 0 && route.Attempts is not null)
            {
                attempt = null;
                foreach (var row in route.Attempts)
                    if (string.Equals(row.Endpoint, endpoint, StringComparison.Ordinal))
                    {
                        attempt = row;
                        break;
                    }
            }
            // Compare retained failure tokens before accepting a recovered route.
            if (waiting)
            {
                if (baseline is { } previous)
                {
                    if (
                        attempt is not { } current
                        || current.IntentId != previous.IntentId
                        || current.LastFailureId != previous.LastFailureId
                    )
                        throw new ZlinkSubmitException(ZlinkSubmitException.ErrorCode.NotConnected);
                }
                else if (attempt is { LastFailureId: not 0 })
                    throw new ZlinkSubmitException(ZlinkSubmitException.ErrorCode.NotConnected);
            }
            if (route.ReadyGeneration == 0)
            {
                if (
                    route.SelectedGeneration == 0
                    && route.Attempt is { State: ConnectionAttemptState.AttemptFailed }
                )
                    throw new ZlinkSubmitException(ZlinkSubmitException.ErrorCode.NotConnected);
                baseline ??= attempt;
                endpoint ??= route.Endpoint ?? attempt?.Endpoint;
                waiting = true;
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

    private static ZLinkFrameworkException Exhausted() =>
        new(
            ZLinkFrameworkErrorKind.DeadlineExceeded,
            "Request deadline elapsed before binding submission.",
            ZLinkRetryAdvice.RetryAfterBackoff
        );
}

// MeshNode owns the selected identity and readiness; waiting callers use its topology edge.
internal readonly record struct ZLinkPeerRouteObservation(
    ulong SelectedGeneration,
    ulong ReadyGeneration,
    Task Changed,
    ConnectionAttempt? Attempt = null,
    string? Endpoint = null,
    IReadOnlyList<ConnectionAttempt>? Attempts = null
);
