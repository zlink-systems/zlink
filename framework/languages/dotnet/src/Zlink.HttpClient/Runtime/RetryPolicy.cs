/* SPDX-License-Identifier: Apache-2.0 */

namespace Zlink.HttpClient.Runtime;

/// <summary>
///     Applies the wrapper's retry policy around a single request attempt: streaming requests are never
///     retried (they cannot be rewound), each attempt is bounded by the effective timeout, and only
///     retriable transport failures (timeout, connection errors) are retried with full-jitter exponential backoff. Timeouts
///     surface as <see cref="ZLinkFrameworkErrorKind.DeadlineExceeded" /> and connection failures as
///     <see cref="ZLinkFrameworkErrorKind.Unavailable" />. Both retain the transport exception as their inner exception.
///     Separated from
///     <see cref="HttpClientRuntime" /> so the retry/timeout policy is independent of handler construction.
/// </summary>
internal sealed class RetryPolicy(HttpClientOptions options)
{
    // Exponential backoff with full jitter: base 50ms, doubling per attempt, capped at 1s.
    // Fixed delays synchronize retries from many clients against an ailing server.
    private static TimeSpan DelayFor(int attempt)
    {
        var ceilingMs = Math.Min(1000, 50 << Math.Min(attempt, 5));
        return TimeSpan.FromMilliseconds(Random.Shared.Next(0, ceilingMs + 1));
    }

    public async ValueTask<RawHttpResponse> ExecuteAsync(
        HttpRequestSpec request,
        Func<HttpRequestSpec, CancellationToken, ValueTask<RawHttpResponse>> perform,
        CancellationToken cancellationToken)
    {
        var maxRetries = request.IsStreaming ? 0 : options.RetryAttempts;
        var timeout = request.Timeout ?? options.Timeout;

        for (var attempt = 0;; attempt++)
        {
            Exception failure;
            try
            {
                using var timeoutCts = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
                timeoutCts.CancelAfter(timeout);
                return await perform(request, timeoutCts.Token).ConfigureAwait(false);
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                throw; // caller cancellation, not a timeout
            }
            catch (OperationCanceledException ex)
            {
                failure = new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.DeadlineExceeded,
                    "HTTP request exceeded timeout",
                    ZLinkRetryAdvice.RetryAfterBackoff,
                    new TimeoutException("HTTP request exceeded timeout", ex));
            }
            catch (ZLinkFrameworkException ex)
            {
                if (ex.RetryAdvice != ZLinkRetryAdvice.DoNotRetry && attempt < maxRetries)
                    failure = ex;
                else
                    throw;
            }
            catch (HttpRequestException ex)
            {
                failure = new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Unavailable,
                    ex.Message,
                    ZLinkRetryAdvice.RetryAfterBackoff,
                    ex);
            }
            catch (System.Net.Sockets.SocketException ex)
            {
                failure = new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Unavailable,
                    ex.Message,
                    ZLinkRetryAdvice.RetryAfterBackoff,
                    ex);
            }
            catch (IOException ex)
            {
                failure = new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Unavailable,
                    ex.Message,
                    ZLinkRetryAdvice.RetryAfterBackoff,
                    ex);
            }

            if (attempt < maxRetries)
            {
                await Task.Delay(DelayFor(attempt), cancellationToken).ConfigureAwait(false);
                continue;
            }

            throw failure;
        }
    }
}
