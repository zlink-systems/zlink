namespace Zlink.Framework.Runtime.Messaging;

// Actor model §sender replay: only durable lifecycle callers enter this owner.
// The binding owns admission and HANDOVER completion; received envelopes leave
// replay before decoding, including rejected and malformed terminal replies.
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
        var admitted = false;
        Exception? lastFailure = null;
        while (true)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var remaining = timeout - timeProvider.GetElapsedTime(startTimestamp);
            if (remaining <= TimeSpan.Zero)
            {
                // The entry's owner confirms expiration. A consumed stage budget
                // prevents another submit but does not independently take terminal authority.
                if (terminal is not null)
                    await terminal.ConfigureAwait(false);
                else if (cancellationToken.CanBeCanceled)
                    await CancellationOnly.WaitAsync(cancellationToken).ConfigureAwait(false);
                throw Exhausted(admitted || requestAdmission?.IsAdmitted == true, lastFailure);
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
                            throw Exhausted(admitted, lastFailure);
                    }
                }
                catch (OperationCanceledException)
                {
                    await Task.CompletedTask.ConfigureAwait(ConfigureAwaitOptions.ForceYielding);
                    throw;
                }
                continue;
            }
            try
            {
                // This source has no deadline. It is the binding contract's
                // generation-retirement signal, not the logical timeout owner.
                using var attempt = CancellationTokenSource.CreateLinkedTokenSource(
                    cancellationToken
                );
                var submittedGeneration = route.ReadyGeneration;
                var reply = submit(wire, remaining, attempt.Token).AsTask();
                while (!reply.IsCompleted)
                {
                    if (terminal is null)
                        await Task.WhenAny(reply, route.Changed).ConfigureAwait(false);
                    else
                    {
                        var observed = await Task.WhenAny(reply, route.Changed, terminal)
                            .ConfigureAwait(false);
                        if (ReferenceEquals(observed, terminal))
                        {
                            _ = DiscardLateReplyAsync(reply);
                            await terminal.ConfigureAwait(false);
                            throw Exhausted(admitted, lastFailure);
                        }
                    }
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

    private static async Task DiscardLateReplyAsync<T>(Task<T> reply)
    {
        try
        {
            if (await reply.ConfigureAwait(false) is IReadOnlyList<Message> parts)
                ZLinkMessageParts.DisposeAll(parts);
        }
        catch (Exception error)
        {
            ZLinkFrameworkDebugLog.TaskFailure("durable-late-reply", error);
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
