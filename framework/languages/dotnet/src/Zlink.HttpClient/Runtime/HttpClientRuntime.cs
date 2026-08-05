/* SPDX-License-Identifier: Apache-2.0 */

using SystemHttpClient = System.Net.Http.HttpClient;

namespace Zlink.HttpClient.Runtime;

/// <summary>
///     Owns the underlying <see cref="System.Net.Http.HttpClient" /> and wires the request performer to
///     the retry policy. Handler/TLS construction lives in <see cref="HttpTransportFactory" /> and the
///     retry/timeout loop in <see cref="RetryPolicy" />. Mirrors the C++ <c>http_client_runtime.cpp</c>.
/// </summary>
internal sealed class HttpClientRuntime : IDisposable
{
    private readonly CookieJar _cookieJar = new();
    private readonly SystemHttpClient _httpClient;
    private readonly RequestPerformer _performer;
    private readonly RetryPolicy _retryPolicy;
    private readonly HttpTransport _transport;

    public HttpClientRuntime(HttpClientOptions options)
    {
        Options = options;
        _transport = HttpTransportFactory.Create(options);
        // disposeHandler: false keeps explicit handler ownership here, so Dispose() releases the
        // transport exactly once (the HttpClient does not also dispose it).
        _httpClient = new SystemHttpClient(_transport.Handler, false)
        {
            // Per-attempt timeout is enforced with a CancellationToken in RetryPolicy, not here.
            Timeout = Timeout.InfiniteTimeSpan
        };
        _performer = new RequestPerformer(options, _cookieJar, _httpClient);
        _retryPolicy = new RetryPolicy(options);
    }

    internal HttpClientOptions Options { get; }

    public void Dispose()
    {
        _httpClient.Dispose();
        _transport.Dispose();
    }

    /// <summary>
    ///     Executes the request, applying the retry policy. The submission API is the caller's native
    ///     <c>Task</c>; no thread is parked while the request is in flight.
    /// </summary>
    public ValueTask<RawHttpResponse> ExecuteAsync(HttpRequestSpec request, CancellationToken cancellationToken)
    {
        return _retryPolicy.ExecuteAsync(request, _performer.PerformAsync, cancellationToken);
    }
}
