namespace Zlink.Framework.Runtime.Messaging;

// Actor model §sender replay: only durable lifecycle callers enter this owner.
// The binding owns admission and HANDOVER completion; received envelopes leave
// replay before decoding, including rejected and malformed terminal replies.
internal static class ZLinkDurableRequest
{
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
        ZLinkRequestAdmission? requestAdmission = null
    )
    {
        timeProvider ??= TimeProvider.System;
        var admitted = false;
        Exception? lastFailure = null;
        while (true)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var remaining = timeout - timeProvider.GetElapsedTime(startTimestamp);
            if (remaining <= TimeSpan.Zero)
                throw Exhausted(admitted || requestAdmission?.IsAdmitted == true, lastFailure);
            var route = observeRoute();
            if (route.ReadyGeneration == 0)
            {
                try
                {
                    using var readiness = new ZLinkRequestCompletion<bool>(cancellationToken);
                    _ = ObserveReadinessAsync(
                        route.Changed.WaitAsync(remaining, timeProvider, cancellationToken),
                        readiness
                    );
                    await readiness.Task.ConfigureAwait(false);
                }
                catch (TimeoutException)
                {
                    throw Exhausted(admitted || requestAdmission?.IsAdmitted == true, lastFailure);
                }
                continue;
            }
            try
            {
                using var attempt = CancellationTokenSource.CreateLinkedTokenSource(
                    cancellationToken
                );
                var submittedGeneration = route.ReadyGeneration;
                var reply = submit(wire, remaining, attempt.Token).AsTask();
                while (!reply.IsCompleted)
                {
                    await Task.WhenAny(reply, route.Changed).ConfigureAwait(false);
                    if (reply.IsCompleted)
                        break;
                    var next = observeRoute();
                    if (next.SelectedGeneration != submittedGeneration)
                    {
                        await attempt.CancelAsync().ConfigureAwait(false);
                        break;
                    }
                    route = next;
                }
                try
                {
                    return await reply.ConfigureAwait(false);
                }
                catch (OperationCanceledException)
                    when (attempt.IsCancellationRequested
                        && !cancellationToken.IsCancellationRequested
                    )
                {
                    admitted |= requestAdmission?.IsAdmitted == true;
                }
            }
            catch (Exception error) when (CanReplay(error, out var requestAdmitted))
            {
                admitted |= requestAdmitted;
                lastFailure = error;
            }
        }
    }

    private static async Task ObserveReadinessAsync(
        Task changed,
        ZLinkRequestCompletion<bool> completion
    )
    {
        try
        {
            await changed.ConfigureAwait(false);
            completion.Complete(true);
        }
        catch (Exception error)
        {
            completion.Fail(error);
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

    private static ZLinkFrameworkException Exhausted(bool admitted, Exception? cause) =>
        new(
            admitted
                ? ZLinkFrameworkErrorKind.DeadlineExceeded
                : ZLinkFrameworkErrorKind.Unavailable,
            admitted
                ? "Durable request reply was not received before its deadline."
                : "Durable request was not admitted before its deadline.",
            ZLinkRetryAdvice.RetryAfterBackoff,
            cause
        );
}

// MeshNode owns the selected identity and readiness; consumers retain only the
// generation used by their attempt and await the existing topology edge.
internal readonly record struct ZLinkPeerRouteObservation(
    ulong SelectedGeneration,
    ulong ReadyGeneration,
    Task Changed
);
