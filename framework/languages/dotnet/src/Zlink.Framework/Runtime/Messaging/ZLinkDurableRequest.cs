using System.Diagnostics;

namespace Zlink.Framework.Runtime.Messaging;

// Actor model §sender replay: only durable lifecycle callers enter this owner.
// The binding owns admission and HANDOVER completion; received envelopes leave
// replay before decoding, including rejected and malformed terminal replies.
internal static class ZLinkDurableRequest
{
    internal static async ValueTask<IReadOnlyList<Message>> RequestAsync(
        IReadOnlyList<ReadOnlyMemory<byte>> wire,
        long startTimestamp,
        TimeSpan timeout,
        Func<IReadOnlyList<ReadOnlyMemory<byte>>, TimeSpan, CancellationToken,
            ValueTask<IReadOnlyList<Message>>> submit,
        CancellationToken cancellationToken)
    {
        var admitted = false;
        Exception? lastFailure = null;
        while (true)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var remaining = timeout - Stopwatch.GetElapsedTime(startTimestamp);
            if (remaining <= TimeSpan.Zero)
                throw Exhausted(admitted, lastFailure);
            try
            {
                return await submit(wire, remaining, cancellationToken)
                    .ConfigureAwait(false);
            }
            catch (Exception error) when (CanReplay(error, out var requestAdmitted))
            {
                admitted |= requestAdmitted;
                lastFailure = error;
            }

            remaining = timeout - Stopwatch.GetElapsedTime(startTimestamp);
            if (remaining <= TimeSpan.Zero)
                throw Exhausted(admitted, lastFailure);
            await Task.Delay(
                    remaining < TimeSpan.FromMilliseconds(10)
                        ? remaining : TimeSpan.FromMilliseconds(10),
                    cancellationToken)
                .ConfigureAwait(false);
        }
    }

    private static bool CanReplay(Exception error, out bool admitted)
    {
        // Framework mappers may wrap a typed binding outcome, but an error
        // kind alone cannot establish the binding's admission phase.
        if (error is ZLinkFrameworkException { InnerException: { } cause })
            error = cause;
        admitted = error is ZlinkRequestException;
        return error is ZlinkRequestException
        {
            Result: ZlinkRequestException.ErrorCode.NotConnected
                or ZlinkRequestException.ErrorCode.TimedOut
        } or ZlinkSubmitException
        {
            Result: ZlinkSubmitException.ErrorCode.NotConnected
                or ZlinkSubmitException.ErrorCode.NotFound
                or ZlinkSubmitException.ErrorCode.Backpressured
                or ZlinkSubmitException.ErrorCode.NotAdmitted
        };
    }

    private static ZLinkFrameworkException Exhausted(bool admitted, Exception? cause) =>
        new(admitted ? ZLinkFrameworkErrorKind.DeadlineExceeded
                : ZLinkFrameworkErrorKind.Unavailable,
            admitted ? "Durable request reply was not received before its deadline."
                : "Durable request was not admitted before its deadline.",
            ZLinkRetryAdvice.RetryAfterBackoff,
            cause);
}
