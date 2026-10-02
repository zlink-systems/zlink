namespace Zlink.Framework.Runtime.Messaging;

// Actor model §sender replay: only durable lifecycle callers enter this owner.
// The binding owns admission and HANDOVER completion; received envelopes leave
// replay before decoding, including rejected and malformed terminal replies.
internal static class ZLinkDurableRequest
{
    private static readonly TimeSpan AdmissionPollInterval = TimeSpan.FromMilliseconds(10);

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
        TimeProvider? timeProvider = null
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
                throw Exhausted(admitted, lastFailure);
            try
            {
                return await submit(wire, remaining, cancellationToken).ConfigureAwait(false);
            }
            catch (Exception error) when (CanReplay(error, out var requestAdmitted))
            {
                admitted |= requestAdmitted;
                lastFailure = error;
            }

            remaining = timeout - timeProvider.GetElapsedTime(startTimestamp);
            if (remaining <= TimeSpan.Zero)
                throw Exhausted(admitted, lastFailure);
            await Task.Delay(
                    remaining < AdmissionPollInterval ? remaining : AdmissionPollInterval,
                    timeProvider,
                    cancellationToken
                )
                .ConfigureAwait(false);
        }
    }

    private static bool CanReplay(Exception error, out bool admitted)
    {
        // mapper가 phase를 확정한 Backpressured terminal은 그대로 전달한다.
        // 그 외 typed binding 오류의 기존 replay 분류는 유지한다.
        if (
            error is ZLinkFrameworkException
            {
                InnerException: { } cause
                    and not ZlinkSubmitException
                    {
                        Result: ZlinkSubmitException.ErrorCode.Backpressured
                    }
            }
        )
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
                        or ZlinkSubmitException.ErrorCode.Backpressured
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
