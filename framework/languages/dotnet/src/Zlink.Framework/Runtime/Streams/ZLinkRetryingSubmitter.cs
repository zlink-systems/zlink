using System.Diagnostics;

namespace Zlink.Framework.Runtime.Streams;

internal static class ZLinkRetryingSubmitter
{
    private static readonly TimeSpan RetryDelay = TimeSpan.FromMilliseconds(25);

    public static async ValueTask Async(
        Func<bool> submit,
        TimeSpan timeout,
        string failureMessage,
        CancellationToken cancellationToken)
    {
        var elapsed = Stopwatch.StartNew();
        Exception? lastError = null;
        while (true)
        {
            cancellationToken.ThrowIfCancellationRequested();
            try
            {
                if (submit()) return;
            }
            catch (ZlinkSubmitException error) when (IsRoutePending(error))
            {
                lastError = error;
            }

            if (elapsed.Elapsed >= timeout)
            {
                var message = lastError is ZlinkSubmitException submitError
                    ? $"{failureMessage} Last submit result: {submitError.Result}."
                    : failureMessage;
                throw new InvalidOperationException(message, lastError);
            }

            var remaining = timeout - elapsed.Elapsed;
            await Task.Delay(remaining < RetryDelay ? remaining : RetryDelay, cancellationToken)
                .ConfigureAwait(false);
        }
    }

    public static ValueTask Async(
        Func<SubmitResult> submit,
        TimeSpan timeout,
        string failureMessage,
        CancellationToken cancellationToken)
    {
        return Async(
            () =>
            {
                var result = submit();
                return result switch
                {
                    SubmitResult.Ok => true,
                    SubmitResult.Backpressured or SubmitResult.NotConnected => false,
                    _ => throw new InvalidOperationException(
                        $"{failureMessage} Submit result: {result}.")
                };
            },
            timeout,
            failureMessage,
            cancellationToken);
    }

    private static bool IsRoutePending(ZlinkSubmitException error)
    {
        return error.Result == ZlinkSubmitException.ErrorCode.NotConnected;
    }
}
